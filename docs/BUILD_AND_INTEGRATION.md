# Build and Integration

This repository is an extension source package for a matching public `mstflint` tree, not a standalone replacement for `mstflint`.

Start from `Mellanox/mstflint` tag `v4.35.0-1.9` (validated commit `8c300f7059b92055325cbbf8e8aff4d44b504e4f`) and follow upstream bootstrap/configure prerequisites.

The Reader Core adds:

```text
mlxreg/q3400_reader.cpp
mlxreg/pddr_module_snapshot.h
```

The public validation package also contains three optional probe sources:

```text
probes/ppcnt_persistent_bench.cpp
probes/fec_histogram_probe.cpp
probes/pddr_module_probe.cpp
```

When integrating manually, copy the Reader files and any desired probes into the upstream `mlxreg/` directory, then append `integration/Makefile.am.fragment` to the upstream `mlxreg/Makefile.am` before regenerating the autotools files.

The public `mstflint` baseline only configures `mlxreg`, `adb_parser`, and their related generic ADB tools when `--enable-adb-generic-tools` is enabled. Therefore the validated integration flow is:

```bash
git clone --branch v4.35.0-1.9 https://github.com/Mellanox/mstflint.git
cd mstflint
# copy extension sources into mlxreg/ and append Makefile fragment
./autogen.sh
./configure --disable-inband --enable-adb-generic-tools
make -j2
```

After the build, the expected validation binaries are:

```text
mlxreg/q3400_reader
mlxreg/ppcnt_persistent_bench
mlxreg/pddr_module_probe
mlxreg/fec_histogram_probe
```

Building from the top-level tree is intentional: the `mlxreg` targets depend on libraries from `adb_parser`, `cmdparser`, `mft_utils`, MTCR, `reg_access`, `tools_layouts`, `xz_utils`, and other upstream directories. The three probes are validation-only tools; `q3400_reader` does not depend on them at runtime.

## Software-only checks

Run the built-in aggregation fixture before hardware access:

```bash
./mlxreg/q3400_reader --self-test-aggregation
```

Then validate the synthetic mapping without `--read`:

```bash
./mlxreg/q3400_reader --mapping tests/q3400_phy_map.example.tsv --ports all --channels all
```

The repository also provides `tests/run_software_checks.sh` to run these checks together. The synthetic map contains no real device topology and is not suitable for hardware access.

## CI

`.github/workflows/build.yml` performs the same integration automatically on GitHub-hosted Ubuntu: it checks out the validated public `mstflint` tag, copies the Reader and probe sources into the upstream tree, applies the Makefile fragment, configures the upstream generic ADB tools, builds the integrated tree, verifies that all four binaries were produced, and runs the software-only tests.

The CI workflow does not open PCI devices, load a runtime ADB for a Q3400, or issue hardware Access Register transactions.

## Hardware path

Run `--self-test-aggregation` and mapping validation before using `--read`. Only after validating PCI functions, the target topology mapping, target MFT/NVOS compatibility, and the ADB should the GET-only hardware path be used.

The temporary Reader CLI accepts `counters`, `histogram`, and `module` capabilities. Exit code 0 means no requested read failures, 1 means a fatal setup/argument/schema initialization error, and 2 means a partial result whose JSON still contains successful objects and per-object errors.
