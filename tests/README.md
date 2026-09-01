# Software-only validation fixtures

This directory contains synthetic fixtures only. It does not contain the device-derived 580-object topology map used in the private Q3400 validation environment.

`q3400_phy_map.example.tsv` is intentionally small and synthetic. It exists to exercise mapping validation and logical-channel aggregation without exposing chassis-specific local-port data.

Recommended software-only checks:

```bash
./mlxreg/q3400_reader --self-test-aggregation
./mlxreg/q3400_reader --mapping tests/q3400_phy_map.example.tsv --ports all --channels all
./mlxreg/q3400_reader --mapping tests/q3400_phy_map.example.tsv --ports 1 --channels 1,2,3,4
```

The second and third commands validate selection/mapping only; without `--read` they do not open hardware devices.

Expected topology semantics for the synthetic port 1 fixture:

- `ipil=1` with mapping channels CH1-CH4 derives logical channels 1-4 and logical port `1/1`.
- `ipil=2` with mapping channels CH1-CH4 derives logical channels 5-8 and logical port `1/2`.
- port 2 contains only `ipil=1`, so no empty `2/2` group should be synthesized by aggregation tests.
