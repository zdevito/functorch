// Copyright (c) Facebook, Inc. and its affiliates.
// All rights reserved.
//
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree.

#include <functorch/csrc/BatchRulesHelper.h>
#include <functorch/csrc/PlumbingHelper.h>
#include <ATen/Operators.h>
#include <ATen/core/dispatch/Dispatcher.h>

namespace at { namespace functorch {

static void handleScalarTypePromotion(Tensor& logical_scalar_tensor, Tensor& second) {
  auto result_type = at::native::result_type(logical_scalar_tensor[0], second);
  if (logical_scalar_tensor.scalar_type() != result_type) {
    logical_scalar_tensor = logical_scalar_tensor.to(result_type);
  }
  if (second.scalar_type() != result_type) {
    second = second.to(result_type);
  }
}

std::tuple<Tensor, Tensor> _binary_pointwise_helper(
    const Tensor& tensor, optional<int64_t> tensor_batch_dim,
    const Tensor& other, optional<int64_t> other_batch_dim) {
  // compute max logical rank
  auto tensor_logical_rank = rankWithoutBatchDim(tensor, tensor_batch_dim);
  auto other_logical_rank = rankWithoutBatchDim(other, other_batch_dim);
  auto max_logical_rank = std::max(tensor_logical_rank, other_logical_rank);

  auto tensor_ = moveBatchDimToFront(tensor, tensor_batch_dim);
  auto other_ = moveBatchDimToFront(other, other_batch_dim);

  // In the (0D, ND) case, type promotion semantics are different :/
  auto tensor_is_logical_scalar = (tensor_logical_rank == 0 && tensor_batch_dim.has_value());
  auto other_is_logical_scalar = (other_logical_rank == 0 && other_batch_dim.has_value());
  if (tensor_is_logical_scalar && !other_is_logical_scalar) {
    handleScalarTypePromotion(tensor_, other_);
  }
  if (other_is_logical_scalar && !tensor_is_logical_scalar) {
    handleScalarTypePromotion(other_, tensor_);
  }

  // If the dimensions aren't aligned, we need to line them up.
  // Tensor[B, 3] + Tensor[2, 5, 3] -> Tensor[B, 1, 1, 3] + Tensor[2, 5, 3]
  // Note that only tensors that have a batch dim need to be modified.
  // Tensor[B, 2, 3, 5] + Tensor[5] -> no changes needed
  tensor_ = maybePadToLogicalRank(tensor_, tensor_batch_dim, max_logical_rank);
  other_ = maybePadToLogicalRank(other_, other_batch_dim, max_logical_rank);

  return std::make_tuple(tensor_, other_);
}

template <typename F, F Func, typename... ExtraArgs>
std::tuple<Tensor,optional<int64_t>> _binary_pointwise_batch_rule(
    const Tensor& tensor, optional<int64_t> tensor_batch_dim,
    const Tensor& other, optional<int64_t> other_batch_dim,
    ExtraArgs... extra_args) {

  auto tensor_other = _binary_pointwise_helper(
      tensor, tensor_batch_dim, other, other_batch_dim);
  auto tensor_ = std::get<0>(tensor_other);
  auto other_ = std::get<1>(tensor_other);

  auto result = Func(tensor_, other_, std::forward<ExtraArgs>(extra_args)...);
  return std::make_tuple(result, 0);
}

template <typename A, A a, typename C>
struct BinaryPointwiseBatchRuleHelper;

template <typename F, F Func, typename T1, typename T2, typename... T>
struct BinaryPointwiseBatchRuleHelper<F, Func, typelist<T1, T2, T...>> {
  static std::tuple<Tensor,optional<int64_t>> apply(
      const Tensor& tensor, optional<int64_t> tensor_batch_dim,
      const Tensor& other, optional<int64_t> other_batch_dim,
      T... extra_args) {
    return _binary_pointwise_batch_rule<F, Func, T...>(
        tensor, tensor_batch_dim, other, other_batch_dim,
        std::forward<T>(extra_args)...);
  }
};

#define BINARY_POINTWISE_BATCH_RULE(fn) SINGLE_ARG(\
    BinaryPointwiseBatchRuleHelper<\
      decltype(&fn),\
      &fn,\
      c10::guts::function_traits<decltype(fn)>::parameter_types>::apply)

template <typename A, A a, typename C>
struct BinaryRandomPointwiseBatchRuleHelper;

template <typename F, F Func, typename T1, typename T2, typename... T>
struct BinaryRandomPointwiseBatchRuleHelper<F, Func, typelist<T1, T2, T...>> {
  static Tensor apply(const Tensor& tensor, const Tensor& other, T... extra_args) {
    c10::impl::ExcludeDispatchKeyGuard guard(kVmapModeKey);
    auto maybe_layer = maybeCurrentDynamicLayer();
    auto cur_level = maybe_layer->layerId();
    RandomnessType randomness = maybe_layer->randomness();

    Tensor tensor_value;
    optional<int64_t> tensor_bdim;
    std::tie(tensor_value, tensor_bdim) = unwrapTensorAtLevel(tensor, cur_level);

    Tensor other_value;
    optional<int64_t> other_bdim;
    std::tie(other_value, other_bdim) = unwrapTensorAtLevel(other, cur_level);

    check_randomness(randomness, (tensor_bdim || other_bdim));
    if (randomness == RandomnessType::Different && !tensor_bdim && !other_bdim) {
      auto shape = tensor_value.sizes();
      VmapDimVector shapeVec(shape.begin(), shape.end());
      shapeVec.insert(shapeVec.begin(), maybe_layer->batchSize());
      tensor_value = tensor_value.unsqueeze(0).expand(shapeVec);
      tensor_bdim = 0;
    } else if (randomness == RandomnessType::Same && !tensor_bdim && !other_bdim) {
      return Func(tensor_value, other_value, std::forward<T>(extra_args)...);
    }
    auto res = _binary_pointwise_batch_rule<F, Func, T...>(
      tensor_value, tensor_bdim, other_value, other_bdim,
      std::forward<T>(extra_args)...);
    return makeBatched(std::get<0>(res), std::get<1>(res), cur_level);
  }
};

#define BINARY_RANDOM_POINTWISE_BATCH_RULE(fn) SINGLE_ARG(\
    BinaryRandomPointwiseBatchRuleHelper<\
      decltype(&fn),\
      &fn,\
      c10::guts::function_traits<decltype(fn)>::parameter_types>::apply)

template <typename M, M Meth, typename... ExtraArgs>
void binary_pointwise_inplace_batch_rule(
    Tensor& tensor, optional<int64_t> tensor_batch_dim,
    const Tensor& other, optional<int64_t> other_batch_dim,
    ExtraArgs... extra_args) {
  if (!tensor_batch_dim && other_batch_dim) {
    vmapIncompatibleInplaceError("inplace arithmetic");
  }

  // compute max logical rank
  auto tensor_logical_rank = rankWithoutBatchDim(tensor, tensor_batch_dim);
  auto other_logical_rank = rankWithoutBatchDim(other, other_batch_dim);
  auto max_logical_rank = std::max(tensor_logical_rank, other_logical_rank);

  auto tensor_ = moveBatchDimToFront(tensor, tensor_batch_dim);
  auto other_ = moveBatchDimToFront(other, other_batch_dim);

  // If the dimensions aren't aligned, we need to line them up.
  // Tensor[B, 3] + Tensor[2, 5, 3] -> Tensor[B, 1, 1, 3] + Tensor[2, 5, 3]
  // Note that only tensors that have a batch dim need to be modified.
  // Tensor[B, 2, 3, 5] + Tensor[5] -> no changes needed
  tensor_ = maybePadToLogicalRank(tensor_, tensor_batch_dim, max_logical_rank);
  other_ = maybePadToLogicalRank(other_, other_batch_dim, max_logical_rank);

  (tensor_.*Meth)(other_, std::forward<ExtraArgs>(extra_args)...);
}

template <typename F, F Func>
std::tuple<Tensor,optional<int64_t>> comparison_pointwise_batch_rule(
    const Tensor& tensor, optional<int64_t> tensor_batch_dim,
    const Tensor& other, optional<int64_t> other_batch_dim) {
  // compute max logical rank
  auto tensor_logical_rank = rankWithoutBatchDim(tensor, tensor_batch_dim);
  auto other_logical_rank = rankWithoutBatchDim(other, other_batch_dim);
  auto max_logical_rank = std::max(tensor_logical_rank, other_logical_rank);

  auto tensor_ = moveBatchDimToFront(tensor, tensor_batch_dim);
  auto other_ = moveBatchDimToFront(other, other_batch_dim);

  // If the dimensions aren't aligned, we need to line them up.
  // Tensor[B, 3] + Tensor[2, 5, 3] -> Tensor[B, 1, 1, 3] + Tensor[2, 5, 3]
  // Note that only tensors that have a batch dim need to be modified.
  // Tensor[B, 2, 3, 5] + Tensor[5] -> no changes needed
  tensor_ = maybePadToLogicalRank(tensor_, tensor_batch_dim, max_logical_rank);
  other_ = maybePadToLogicalRank(other_, other_batch_dim, max_logical_rank);

  auto result = Func(tensor_, other_);
  return std::make_tuple( std::move(result), 0 );
}

std::tuple<Tensor,optional<int64_t>> _s_where_batch_rule(
    const Tensor& condition, optional<int64_t> condition_bdim,
    const Tensor& self, optional<int64_t> self_bdim, const Tensor& other, optional<int64_t> other_bdim) {
  auto condition_ = moveBatchDimToFront(condition, condition_bdim);
  auto self_ = moveBatchDimToFront(self, self_bdim);
  auto other_ = moveBatchDimToFront(other, other_bdim);
  return std::make_tuple(at::where(condition_, self_, other_), 0);
}

std::tuple<Tensor,optional<int64_t>> masked_select_batch_rule(
    const Tensor& self, optional<int64_t> self_bdim,
    const Tensor& mask, optional<int64_t> mask_bdim) {
  TORCH_CHECK(!mask_bdim.has_value(),
      "vmap: Attempted to vmap over `mask` in torch.masked_select(self, mask) ",
      "We cannot support this because for each batch this would return a ",
      "differently shaped Tensor. "
      "Please voice your support in https://github.com/pytorch/functorch/issues/256");
  auto self_ = moveBatchDimToFront(self, self_bdim);
  const auto batch_size = self_.size(0);
  const auto self_logical_rank = rankWithoutBatchDim(self, self_bdim);
  const auto max_logical_rank = std::max(self_logical_rank, mask.dim());
  self_ = maybePadToLogicalRank(self_, 0, max_logical_rank);

  // masked_select returns a 1D tensor, so we have to reshape it into 2D
  const auto result = at::masked_select(self_, mask).view({ batch_size, -1 });
  return std::make_tuple(result, 0);
}

Tensor addr_decomposition(
    const Tensor& self, const Tensor& vec1, const Tensor& vec2,
    const Scalar& beta, const Scalar& alpha) {

  auto outer = alpha * vec1.unsqueeze(-1) * vec2.unsqueeze(-2);
  return self * beta + outer;
}

std::tuple<Tensor,optional<int64_t>> cdist_backward_batch_rule(
    const Tensor& grad, optional<int64_t> grad_bdim,
    const Tensor& x1, optional<int64_t> x1_bdim,
    const Tensor& x2, optional<int64_t> x2_bdim,
    const double p,
    const Tensor& cdist, optional<int64_t> cdist_bdim) {

  auto x1_ = x1;
  if (cdist_bdim && !x1_bdim) {
    // We need to make sure that x1 has batch dim if cdist has one
    // otherwise, we get
    // RuntimeError: Function CdistBackward0 returned an invalid gradient at index 1 - got [5]
    // but expected shape compatible with [4, 5]
    auto bs = cdist.size(*cdist_bdim);
    x1_ = ensure_has_bdim(x1, false, bs);
    x1_ = x1_.contiguous();
    x1_bdim = 0;
  }

  // We need to apply the same preprocessing on x1 and x2 as in the forward pass
  // _binary_pointwise_batch_rule
  auto x12 = _binary_pointwise_helper(x1_, x1_bdim, x2, x2_bdim);
  x1_ = std::get<0>(x12);
  auto x2_ = std::get<1>(x12);

  auto grad_ = moveBatchDimToFront(grad, grad_bdim);
  if ((x1_bdim || x2_bdim) && !grad_bdim) {
    // We need to make sure that grad has batch dim if x1 or x2 have one
    // Probably, there is an assumption on the strides.
    // Otherwise grad input contains thrash values, e.g. -7.0816e+29, 7.0816e+29
    auto bs = get_bdim_size2(x1_, 0, x2_, 0);
    grad_ = ensure_has_bdim(grad_, grad_bdim.has_value(), bs);
    grad_ = grad_.contiguous();
  }

  auto out = at::_cdist_backward(grad_, x1_, x2_, p, cdist);

  optional<int64_t> out_bdim = nullopt;
  if (x1_bdim || x2_bdim) {
    out_bdim = 0;
  }

  return std::make_tuple(out, out_bdim);
}

TORCH_LIBRARY_IMPL(aten, FuncTorchVmapMode, m) {
  #define BINARY_RANDOM_POINTWISE(op) \
  m.impl(#op, BINARY_RANDOM_POINTWISE_BATCH_RULE(ATEN_FN(op)));
#define BINARY_RANDOM_POINTWISE2(op, overload) \
  m.impl(#op"."#overload, BINARY_RANDOM_POINTWISE_BATCH_RULE(ATEN_FN2(op, overload)));

  BINARY_RANDOM_POINTWISE2(normal, Tensor_Tensor);
  BINARY_RANDOM_POINTWISE(binomial);
}

TORCH_LIBRARY_IMPL(aten, FT_BATCHED_KEY, m) {
#define BINARY_POINTWISE2(op, overload) \
  VMAP_SUPPORT2(op, overload, BINARY_POINTWISE_BATCH_RULE(ATEN_FN2(op, overload)));
#define BINARY_POINTWISE(op) \
  VMAP_SUPPORT(op, BINARY_POINTWISE_BATCH_RULE(ATEN_FN(op)));
#define UNARY_POINTWISE2(op, overload) \
  VMAP_SUPPORT2(op, overload, BASIC_UNARY_BATCH_RULE(ATEN_FN2(op, overload)));
#define UNARY_POINTWISE(op) \
  VMAP_SUPPORT(op, BASIC_UNARY_BATCH_RULE(ATEN_FN(op)));
#define UNARY_SCALAR_POINTWISE2(op, overload) \
  VMAP_SUPPORT(op, overload, SCALAR_UNARY_BATCH_RULE(ATEN_FN2(op, overload)));

#define BINARY_SCALAR_2(op, tensor_tensor, tensor_scalar) \
  BINARY_POINTWISE2(op, tensor_tensor);\
  UNARY_POINTWISE2(op, tensor_scalar);

// For all 3 combinations of Tensor x Tensor, Tensor x Scalar, Scalar x Tensor
#define BINARY_SCALAR_3(op, tensor_tensor, tensor_scalar, scalar_tensor) \
  BINARY_POINTWISE2(op, tensor_tensor);\
  UNARY_POINTWISE2(op, tensor_scalar);\
  POINTWISE_BOXED(op.scalar_tensor);

#define BINARY_SCALAR_3_Tensor(op, tensor_scalar, scalar_tensor) \
  BINARY_POINTWISE(op);\
  UNARY_POINTWISE2(op, tensor_scalar);\
  POINTWISE_BOXED(op.scalar_tensor);

  // Batching rule registrations start
  POINTWISE_BOXED(__ilshift__.Tensor);
  POINTWISE_BOXED(__ilshift__.Scalar);
  POINTWISE_BOXED(__irshift__.Tensor)
  POINTWISE_BOXED(__irshift__.Scalar)
  BINARY_SCALAR_2(__lshift__, Tensor, Scalar);
  BINARY_SCALAR_2(__rshift__, Tensor, Scalar);

  BINARY_SCALAR_2(add, Tensor, Scalar);
  POINTWISE_BOXED(addcdiv);
  POINTWISE_BOXED(addcmul);
  m.impl("addr", addr_decomposition);
  BINARY_POINTWISE(atan2);
  BINARY_SCALAR_2(bitwise_and, Tensor, Scalar);
  BINARY_POINTWISE2(bitwise_or, Tensor);
  BINARY_POINTWISE2(bitwise_xor, Tensor);
  BINARY_SCALAR_3(bitwise_left_shift, Tensor, Tensor_Scalar, Scalar_Tensor);
  BINARY_SCALAR_3(bitwise_right_shift, Tensor, Tensor_Scalar, Scalar_Tensor);

  UNARY_POINTWISE(clamp);
  POINTWISE_BOXED(clamp.Tensor);
  BINARY_POINTWISE2(clamp_min, Tensor);
  UNARY_POINTWISE(clamp_min);
  POINTWISE_BOXED(clamp_min_);
  BINARY_POINTWISE2(clamp_max, Tensor);
  UNARY_POINTWISE(clamp_max);
  POINTWISE_BOXED(clamp_max_);

  VARIADIC_BDIMS_BOXED(_euclidean_dist);
  // Implementation note: _binary_pointwise_helper performs a dtype promotion if args are scalars,
  // but cdist can't work with scalars, at least 2d tensors.
  BINARY_POINTWISE(_cdist_forward);
  VMAP_SUPPORT(_cdist_backward, cdist_backward_batch_rule);

  // Commented out so we have a test op
  // BINARY_SCALAR_2(copysign, Tensor, Scalar);
  BINARY_SCALAR_2(div, Tensor, Scalar);
  BINARY_SCALAR_2(div, Tensor_mode, Scalar_mode);

  BINARY_POINTWISE(floor_divide);
  UNARY_POINTWISE2(floor_divide, Scalar);

  BINARY_POINTWISE(fmax);
  BINARY_POINTWISE(fmin);
  BINARY_SCALAR_2(fmod, Tensor, Scalar);
  POINTWISE_BOXED(frexp.Tensor);
  BINARY_POINTWISE(heaviside);
  BINARY_POINTWISE(hypot);
  BINARY_POINTWISE(gcd);
  BINARY_POINTWISE(igamma);
  BINARY_POINTWISE(igammac);
  BINARY_POINTWISE(logaddexp);
  BINARY_POINTWISE(logaddexp2);
  POINTWISE_BOXED(lerp.Scalar);
  POINTWISE_BOXED(lerp.Tensor);
  BINARY_POINTWISE(lcm);
  POINTWISE_BOXED(log_sigmoid_forward);
  BINARY_POINTWISE(maximum);
  BINARY_POINTWISE(minimum);

  BINARY_SCALAR_2(mul, Tensor, Scalar);
  BINARY_POINTWISE(nextafter);
  BINARY_SCALAR_3(pow, Tensor_Tensor, Tensor_Scalar, Scalar);
  BINARY_POINTWISE(polar);
  POINTWISE_BOXED(polygamma);
  BINARY_SCALAR_2(sub, Tensor, Scalar);
  BINARY_SCALAR_3(remainder, Tensor, Scalar, Scalar_Tensor);
  BINARY_POINTWISE(rrelu_with_noise);
  BINARY_SCALAR_2(rsub, Tensor, Scalar);

  BINARY_SCALAR_3_Tensor(special_xlog1py, other_scalar, self_scalar);
  BINARY_SCALAR_3_Tensor(special_xlogy, other_scalar, self_scalar);
  BINARY_SCALAR_3_Tensor(special_zeta, other_scalar, self_scalar);

  VMAP_SUPPORT(_s_where, _s_where_batch_rule);

  BINARY_SCALAR_3(xlogy, Tensor, Scalar_Other, Scalar_Self);

  POINTWISE_BOXED(elu_backward);
  BINARY_POINTWISE(hardsigmoid_backward);
  BINARY_POINTWISE(hardtanh_backward);
  BINARY_POINTWISE(hardshrink_backward);
  BINARY_POINTWISE(hardswish_backward);
  // BINARY_POINTWISE(infinitely_differentiable_gelu_backward);
  BINARY_POINTWISE(leaky_relu_backward);
  BINARY_POINTWISE(logit_backward);
  POINTWISE_BOXED(log_sigmoid_backward);
  BINARY_POINTWISE(gelu_backward);
  BINARY_POINTWISE(sigmoid_backward);
  POINTWISE_BOXED(softplus_backward);
  BINARY_POINTWISE(softshrink_backward);
  BINARY_POINTWISE(tanh_backward);
  BINARY_POINTWISE(threshold_backward);

  using TensorScalarInplaceT = Tensor& (Tensor::*)(const Tensor&, const Scalar&) const;
  using ScalarScalarInplaceT = Tensor& (Tensor::*)(const Scalar&, const Scalar&) const;
  using TensorInplaceT = Tensor& (Tensor::*)(const Tensor&) const;
  using ScalarInplaceT = Tensor& (Tensor::*)(const Scalar&) const;
  using CopyT = Tensor& (Tensor::*)(const Tensor&, bool) const;

  POINTWISE_BOXED(add_.Tensor); // just testing
  VMAP_SUPPORT2(add_, Scalar, SINGLE_ARG(unary_inplace_batch_rule<ScalarScalarInplaceT, &Tensor::add_, const Scalar&, const Scalar&>));
  VMAP_SUPPORT2(sub_, Tensor, SINGLE_ARG(binary_pointwise_inplace_batch_rule<TensorScalarInplaceT, &Tensor::sub_, const Scalar&>));
  VMAP_SUPPORT2(sub_, Scalar, SINGLE_ARG(unary_inplace_batch_rule<ScalarScalarInplaceT, &Tensor::sub_, const Scalar&, const Scalar&>));
  VMAP_SUPPORT2(mul_, Tensor, SINGLE_ARG(binary_pointwise_inplace_batch_rule<TensorInplaceT, &Tensor::mul_>));
  VMAP_SUPPORT2(mul_, Scalar, SINGLE_ARG(unary_inplace_batch_rule<ScalarInplaceT, &Tensor::mul_, const Scalar&>));
  VMAP_SUPPORT2(div_, Tensor, SINGLE_ARG(binary_pointwise_inplace_batch_rule<TensorInplaceT, &Tensor::div_>));
  VMAP_SUPPORT2(div_, Scalar, SINGLE_ARG(unary_inplace_batch_rule<ScalarInplaceT, &Tensor::div_, const Scalar&>));
  VMAP_SUPPORT2(clamp_min_, Tensor, SINGLE_ARG(binary_pointwise_inplace_batch_rule<TensorInplaceT, &Tensor::clamp_min_>));
  VMAP_SUPPORT2(clamp_max_, Tensor, SINGLE_ARG(binary_pointwise_inplace_batch_rule<TensorInplaceT, &Tensor::clamp_max_>));
  VMAP_SUPPORT2(masked_fill_, Scalar, SINGLE_ARG(binary_pointwise_inplace_batch_rule<TensorScalarInplaceT, &Tensor::masked_fill_, const Scalar&>));
  VMAP_SUPPORT(copy_, SINGLE_ARG(binary_pointwise_inplace_batch_rule<CopyT, &Tensor::copy_, bool>));

#define COMPARISON_POINTWISE(op) \
  VMAP_SUPPORT2(op, Tensor, \
      SINGLE_ARG(comparison_pointwise_batch_rule<decltype(&ATEN_FN2(op, Tensor)), &at::op>)); \
  UNARY_POINTWISE2(op, Scalar)

  COMPARISON_POINTWISE(eq);
  COMPARISON_POINTWISE(gt);
  COMPARISON_POINTWISE(ge);
  COMPARISON_POINTWISE(le);
  COMPARISON_POINTWISE(lt);
  COMPARISON_POINTWISE(ne);

#undef COMPARISON_POINTWISE
#undef BINARY_POINTWISE2
#undef BINARY_POINTWISE
#undef UNARY_POINTWISE2
#undef UNARY_POINTWISE
#undef UNARY_SCALAR_POINTWISE2
#undef BINARY_SCALAR_3

#define LOGICAL_COMPARISON_POINTWISE(op) \
  VMAP_SUPPORT(op, \
      SINGLE_ARG(comparison_pointwise_batch_rule<decltype(&ATEN_FN(op)), &ATEN_FN(op)>)); \
  VMAP_SUPPORT(op ## _, \
      SINGLE_ARG(binary_pointwise_inplace_batch_rule<TensorInplaceT, &Tensor:: op ## _ >));

  LOGICAL_COMPARISON_POINTWISE(logical_and);
  LOGICAL_COMPARISON_POINTWISE(logical_or);
  LOGICAL_COMPARISON_POINTWISE(logical_xor);

#undef SINGLE_ARG
#undef LOGICAL_COMPARISON_POINTWISE
  VMAP_SUPPORT(masked_select, masked_select_batch_rule);
}

}}
