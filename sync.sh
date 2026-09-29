#!/bin/sh
# Sync sources to big4 and rebuild the extension there.
rsync -a --exclude venv --exclude simdjson --exclude simdjson-data --exclude build --exclude '*.so' ./ big4:fastpysimdjson/ && ssh big4 'cd ~/fastpysimdjson && ./venv/bin/python setup.py build_ext --inplace --force -q 2>&1 | grep -E "error|warning" ; true'
