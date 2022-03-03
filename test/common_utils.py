# Copyright (c) Facebook, Inc. and its affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

import itertools
import torch
import functorch
from functorch import vmap
import torch.utils._pytree as pytree
from functorch_lagging_op_db import functorch_lagging_op_db
from functorch_additional_op_db import additional_op_db
from torch.testing._internal.common_methods_invocations import DecorateInfo
import os
import unittest
from torch.testing._internal.common_device_type import toleranceOverride

IS_FBCODE = os.getenv('FUNCTORCH_TEST_FBCODE') == '1'


def loop(op, in_dims, out_dim, batch_size, *batched_args, **kwarg_values):
    outs = []
    for idx in range(batch_size):
        flat_args, args_spec = pytree.tree_flatten(batched_args)
        flat_dims, dims_spec = pytree.tree_flatten(in_dims)
        assert(args_spec == dims_spec)
        new_args = [a.select(in_dim, idx) if in_dim is not None else a for a, in_dim in zip(flat_args, flat_dims)]
        out = op(*pytree.tree_unflatten(new_args, args_spec), **kwarg_values)
        outs.append(out)

    loop_out = []
    if isinstance(outs[0], torch.Tensor):
        loop_out = torch.stack(outs)
    else:
        for idx in range(len(outs[0])):
            loop_out.append(torch.stack([i[idx] for i in outs], out_dim))
    return loop_out


def get_exhaustive_batched_inputs(arg_values, kwarg_values, batch_size=3, bdims=(0, -1), for_batch_norm=False):
    assert bdims == (0,) or bdims == (0, -1)

    def add_batch_dim(arg, bdim, batch_size=3):
        assert bdim == 0 or bdim == -1
        if isinstance(arg, torch.Tensor):
            if bdim == 0:
                shape = [1] * len(arg.shape)
                shape.insert(bdim, batch_size)
                return (arg.repeat(shape), bdim)
            if bdim == -1:
                arg = arg.unsqueeze(-1).expand(*arg.shape, batch_size).contiguous()
                return (arg, bdim)
            assert False
        else:
            return (arg, None)
    for bdim in bdims:
        batch_choices = []

        def add_batch_choices(a):
            if isinstance(a, torch.Tensor):
                batched_val = add_batch_dim(a, bdim, batch_size)
                batch_choices.append((batched_val, (a, None)))
            else:
                batch_choices.append(((a, None),))

        flat_args, arg_spec = pytree.tree_flatten(tuple(arg_values))
        if for_batch_norm:
            # Batch norm is unique because the running_mean and running_var are updated in place.
            # Therefore, they cannot be unbatched if the input is batched. The case where both are
            # unbatched is added at the end
            if len(flat_args) >= 3:
                add_batch_choices(flat_args[0])  # input can be batched or unbatched
                batch_choices.append((add_batch_dim(flat_args[1], bdim, batch_size),))  # running_mean must be batched
                batch_choices.append((add_batch_dim(flat_args[2], bdim, batch_size),))  # running_var must be batched
                orig_flat_args = flat_args
                flat_args = orig_flat_args[3:]
            else:
                # TODO: None defaults in instance norm create empty tensors that are written to and mean that we must
                # have unbatched inputs. None in the running mean/running var shouldn't make a tensor
                batch_choices.append(((flat_args[0], None),))  # input must be unbatched
                if len(flat_args) == 2:
                    batch_choices.append((add_batch_dim(flat_args[1], bdim, batch_size),))
                orig_flat_args = flat_args
                flat_args = []

        for arg in flat_args:
            add_batch_choices(arg)

        for batched_values in itertools.product(*batch_choices):
            batched_args, in_dims = zip(*batched_values)

            if all([i is None for i in in_dims]):
                continue

            yield pytree.tree_unflatten(batched_args, arg_spec), pytree.tree_unflatten(in_dims, arg_spec), kwarg_values

        if for_batch_norm and len(orig_flat_args) >= 2:
            # Adds the case where input, running_mean, and running_var are all unbatched
            batch_choices[0] = ((orig_flat_args[0], None),)
            batch_choices[1] = ((orig_flat_args[1], None),)
            if len(orig_flat_args) >= 3:
                batch_choices[2] = ((orig_flat_args[2], None),)
            for batched_values in itertools.product(*batch_choices):
                batched_args, in_dims = zip(*batched_values)

                if all([i is None for i in in_dims]):
                    continue

                batched_args_tuple = pytree.tree_unflatten(batched_args, arg_spec)
                in_dims_tuple = pytree.tree_unflatten(in_dims, arg_spec)
                yield batched_args_tuple, in_dims_tuple, kwarg_values


def get_exhaustive_batched_inputs_for_batch_norm(arg_values, kwarg_values, batch_size=3, bdims=(0, -1)):
    return get_exhaustive_batched_inputs(arg_values, kwarg_values,
                                         batch_size=batch_size, bdims=bdims, for_batch_norm=True)


def get_fallback_and_vmap_exhaustive(op, arg_values, kwarg_values, opinfo=None, compute_loop_out=True, bdims=(0, -1)):
    out_dim = 0
    batch_size = 4
    generator = get_exhaustive_batched_inputs(arg_values, kwarg_values, batch_size, bdims=bdims)
    batch_norm_fns = ("nn.functional.batch_norm", "nn.functional.instance_norm")  # instance norm calls batch norm
    if opinfo is not None and opinfo.name in batch_norm_fns:
        generator = get_exhaustive_batched_inputs_for_batch_norm(arg_values, kwarg_values, batch_size, bdims=bdims)
    for batched_args, in_dims, kwarg_values in generator:
        if compute_loop_out:
            loop_out = loop(op, in_dims, out_dim, batch_size, *batched_args, **kwarg_values)
        else:
            loop_out = None
        # Used for debugging the resulting operations
        # from functorch import make_fx
        # def f(a):
        #     return op(a)
        # t = make_fx(vmap(f, in_dims=in_dims, out_dims=out_dim))(*batched_args, **kwarg_values)
        # print(in_dims, [arg.shape for arg in batched_args], kwarg_values)
        batched_out = vmap(op, in_dims=in_dims, out_dims=out_dim)(*batched_args, **kwarg_values)
        yield (loop_out, batched_out)

        # Tests case where we dispatch to a batching rule with no bdims
        # This should be handled by autogenerated plumbing. For vmap support
        # added via a manual plumbing you may need to handle this specially.
        def add_bdim_if_tensor(x):
            if isinstance(x, torch.Tensor):
                return x.unsqueeze(1)
            return x

        def f(dummy, *args, **kwargs):
            return op(*args, **kwargs)

        dummy = torch.ones(batch_size, 1)
        expected = pytree.tree_map(add_bdim_if_tensor, batched_out)

        inner_in_dims = (0,) + pytree.tree_map(lambda x: None, in_dims)
        outer_in_dims = (0,) + in_dims
        output = vmap(vmap(f, inner_in_dims), outer_in_dims)(dummy, *batched_args, **kwarg_values)
        yield (expected, output)


def opinfo_in_dict(opinfo, d):
    return (opinfo.name in d) or (f'{opinfo.name}.{opinfo.variant_test_name}' in d)


def xfail(op_name, variant_name=None, *, device_type=None, dtypes=None):
    return (op_name, variant_name, device_type, dtypes, True)

# TODO: this doesn't work in python < 3.8


def skip(op_name, variant_name=None, *, device_type=None, dtypes=None):
    return (op_name, variant_name, device_type, dtypes, False)


def skipOps(test_case_name, base_test_name, to_skip):
    all_opinfos = functorch_lagging_op_db + additional_op_db
    for xfail in to_skip:
        op_name, variant_name, device_type, dtypes, expected_failure = xfail
        if variant_name is None:
            # match all variants
            matching_opinfos = [o for o in all_opinfos if o.name == op_name]
            assert len(matching_opinfos) >= 1, f"Couldn't find OpInfo for {xfail}"
        else:
            matching_opinfos = [o for o in all_opinfos
                                if o.name == op_name and o.variant_test_name == variant_name]
            assert len(matching_opinfos) >= 1, f"Couldn't find OpInfo for {xfail}"
        for opinfo in matching_opinfos:
            decorators = list(opinfo.decorators)
            if expected_failure:
                decorators.append(DecorateInfo(unittest.expectedFailure,
                                               test_case_name, base_test_name,
                                               device_type=device_type, dtypes=dtypes))
            else:
                decorators.append(DecorateInfo(unittest.skip("Skipped!"),
                                               test_case_name, base_test_name,
                                               device_type=device_type, dtypes=dtypes))
            opinfo.decorators = tuple(decorators)

    # This decorator doesn't modify fn in any way
    def wrapped(fn):
        return fn
    return wrapped


def tol2(op_name, variant_name, override_dct, *, device_type=None):
    return (op_name, variant_name, override_dct, device_type)


def tol1(op_name, override_dct, *, device_type=None):
    return tol2(op_name, '', override_dct, device_type=device_type)


def opsToleranceOverride(test_case_name, base_test_name, overrides):
    all_opinfos = functorch_lagging_op_db + additional_op_db
    for override in overrides:
        print(override)
        op_name, variant_name, override, device_type = override
        matching_opinfos = [o for o in all_opinfos
                            if o.name == op_name and o.variant_test_name == variant_name]
        assert len(matching_opinfos) == 1, f"Couldn't find OpInfo for {override}"
        opinfo = matching_opinfos[0]
        decorators = list(opinfo.decorators)
        decorators.append(DecorateInfo(
            toleranceOverride(override),
            test_case_name, base_test_name, device_type=device_type))
        opinfo.decorators = tuple(decorators)

    # This decorator doesn't modify fn in any way
    def wrapped(fn):
        return fn
    return wrapped


class DisableVmapFallback:
    def __enter__(self):
        self.prev_state = functorch._C._is_vmap_fallback_enabled()
        functorch._C._set_vmap_fallback_enabled(False)

    def __exit__(self, *ignored):
        functorch._C._set_vmap_fallback_enabled(self.prev_state)


def check_vmap_fallback(test_case, thunk, opinfo, dry_run=False):
    try:
        with DisableVmapFallback():
            thunk()
    except Exception:
        if not dry_run:
            raise
        if opinfo.variant_test_name:
            print(f"xfail('{opinfo.name}', '{opinfo.variant_test_name}'),")
        else:
            print(f"xfail('{opinfo.name}'),")
