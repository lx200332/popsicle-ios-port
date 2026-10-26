#!/bin/bash
cd <BUILD>/mu-silicium || exit 1
echo "===== Building.md ====="
cat Building.md
echo
echo "===== pip-requirements.txt ====="
cat pip-requirements.txt
