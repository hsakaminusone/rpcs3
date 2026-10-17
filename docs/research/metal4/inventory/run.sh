#!/bin/sh
# Regenerate and verify the RSX state-space inventory.
#
#   ./run.sh            # regenerate, test, report coverage (gaps are non-fatal)
#   ./run.sh --strict   # additionally fail unless curation is complete
#
# Steps: generate facts -> unit tests -> disposition validation -> doc lint.
set -eu
cd "$(dirname "$0")"

echo "== generate =="
python3 generate.py

echo "== tests =="
python3 -m unittest discover -p 'test_*.py'

echo "== tests (state-space contract suite) =="
python3 -m unittest discover -s tests -p 'test_*.py'

echo "== validate =="
python3 validate.py "$@"

echo "== lint docs =="
python3 lint_docs.py "$@"

echo "== validate docs =="
python3 validate_docs.py "$@"

echo "== ok =="
