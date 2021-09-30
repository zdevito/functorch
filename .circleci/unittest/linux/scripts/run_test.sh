#!/usr/bin/env bash

set -e

. venv/bin/activate
mkdir test-results
eval "$(./conda/bin/conda shell.bash hook)"
conda activate ./env

python -m torch.utils.collect_env

# test_functorch_lagging_op_db.py: Only run this locally because it checks
# the functorch lagging op db vs PyTorch's op db.
pytest -v --ignore=test/test_functorch_lagging_op_db.py --junitxml=test-results/junit.xml test/ || exit 255
