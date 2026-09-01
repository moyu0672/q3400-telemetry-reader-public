#!/usr/bin/env bash
set -euo pipefail

READER="${1:-./mlxreg/q3400_reader}"
MAP="${2:-tests/q3400_phy_map.example.tsv}"

"$READER" --self-test-aggregation >/dev/null
"$READER" --mapping "$MAP" --ports all --channels all >/dev/null
"$READER" --mapping "$MAP" --ports 1 --channels 1,2,3,4 >/dev/null

echo "software-only checks passed"
