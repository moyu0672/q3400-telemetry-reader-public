# Q3400 Read-only Telemetry Reader

[![build](https://github.com/moyu0672/q3400-telemetry-reader-public/actions/workflows/build.yml/badge.svg)](https://github.com/moyu0672/q3400-telemetry-reader-public/actions/workflows/build.yml)

An independent, high-throughput, GET-only telemetry prototype for NVIDIA Q3400 built against the public NVIDIA/Mellanox `mstflint` Access Register stack.

This repository contains the project-specific Reader source, validation probes, synthetic software-only fixtures, CI integration, and public engineering documentation. It intentionally does **not** redistribute a device-derived full topology map, runtime ADB copy, compiled binary, raw device logs, or presentation/export code.

> This is not an official NVIDIA product or support tool. NVIDIA, Mellanox, Q3400, NVOS, and related names are used only to identify the tested platform and upstream software.

## What this project adds

- Persistent Access Register sessions instead of one CLI process per register read.
- Q3400 multi-plane telemetry object model.
- PPCNT physical-layer counters (`grp=0x16`, GET only, `clr=0`).
- GET-only FEC histogram reads using PPHCR + PPCNT `grp=0x23`.
- PDDR page 3 module/DDM snapshots.
- Structured raw output plus in-memory physical/logical-port aggregation.
- Explicit partial-failure handling and read-only safety boundaries.
- Small validation probes that isolate PPCNT, histogram, and module/status register paths.
- Synthetic mapping fixtures and software-only self-tests.
- GitHub Actions integration that builds against the validated public `mstflint` baseline without accessing Q3400 hardware.

## Measured results

Measured on one tested Q3400/NVOS environment; these are not guarantees for other systems:

| Workload | Measured result |
|---|---:|
| Persistent PPCNT GET | ~5.15 ms / GET |
| Single persistent worker | ~194 GET/s |
| Four-device aggregate | ~466.9 GET/s |
| 580-channel COUNTERS hot scan | ~1.286 s |
| 580 COUNTERS + 73 MODULE hot scan | ~1.754 s |
| Single physical port: COUNTERS + MODULE + HISTOGRAM | ~63 ms hot |
| Full COUNTERS + MODULE + HISTOGRAM | ~2.8–2.9 s hot |

The speedup comes from avoiding repeated process startup, ADB parsing, and MST/device initialization, then reusing persistent device/parser contexts. It does not imply that an individual hardware register transaction itself is orders of magnitude faster.

## Upstream baseline

This work was developed against the public `mstflint` source tree:

- Upstream: `https://github.com/Mellanox/mstflint`
- Tag: `v4.35.0-1.9`
- Commit: `8c300f7059b92055325cbbf8e8aff4d44b504e4f`

The Reader expects the `mstflint` access/parser stack, including `MlxRegLib`, `RegAccessParser`, `maccess_reg`, `mtcr`, and the switch Access Register ADB/layouts. The upstream source tree itself is not copied into this repository.

## Repository layout

```text
mlxreg/
  q3400_reader.cpp
  pddr_module_snapshot.h

probes/
  README.md
  ppcnt_persistent_bench.cpp
  fec_histogram_probe.cpp
  pddr_module_probe.cpp

tests/
  README.md
  q3400_phy_map.example.tsv
  run_software_checks.sh

integration/
  Makefile.am.fragment

.github/workflows/
  build.yml

docs/
  ARCHITECTURE.md
  BENCHMARK_RESULTS.md
  REGISTER_DATA_DICTIONARY.md
  SOURCE_PATHS.md
  Q3400_TOPOLOGY_MAPPING.md
  BUILD_AND_INTEGRATION.md
  UPSTREAM_AND_LICENSE.md
```

## Validation layers

The project intentionally separates three kinds of evidence:

1. `q3400_reader` is the Reader Core prototype.
2. `probes/` contains small hardware-facing experiments used to isolate one register path at a time. They are not Reader Core features.
3. `tests/` and CI exercise mapping/aggregation/build behavior without Q3400 hardware.

The CI workflow checks out the validated public `mstflint` tag, integrates the Reader and probes, builds all four binaries, and runs the software-only Reader self-tests. It does not open PCI devices or issue hardware Access Register transactions.

## Topology mapping policy

The tested Reader uses a TSV topology mapping with columns:

```text
label_port  ipil  channel  split  pciconf  local_port  module  sub_module
```

The complete device-derived 580-object mapping used in the private validation environment is intentionally **not distributed in this public repository**. See [`docs/Q3400_TOPOLOGY_MAPPING.md`](docs/Q3400_TOPOLOGY_MAPPING.md) for the schema, logical-channel rule, validation requirements, and why users must supply a mapping validated for their own chassis/firmware environment.

`tests/q3400_phy_map.example.tsv` is synthetic and exists only for software validation. It must not be treated as a hardware mapping.

The current Reader source also contains a tested `pciconf -> PCI BDF` mapping. PCI enumeration is environment-specific; validate or adapt it before hardware use.

## Safety

The public Reader baseline is read-only:

- no Access Register SET path;
- no counter clear;
- no histogram clear;
- no reset or reboot;
- no device configuration changes.

`COUNTERS` uses PPCNT `grp=0x16` with `clr=0`; `HISTOGRAM_READ` uses PPHCR GET plus PPCNT `grp=0x23` with `clr=0`; `MODULE_SNAPSHOT` uses PDDR page 3 GET.

The validation probes are also GET-only. Some probe modes intentionally inspect registers that are outside current Reader Core scope, such as PDDR page 9 or PMAOS; their presence does not mean those capabilities are implemented in the Reader.

## Build / integration

This repository is an extension source package, not a standalone copy of `mstflint`. To build it, start from the matching public `mstflint` baseline and integrate the files as described in [`docs/BUILD_AND_INTEGRATION.md`](docs/BUILD_AND_INTEGRATION.md).

Do not run the hardware path until the topology mapping, PCI enumeration, ADB, target MFT/NVOS version, and read-only transaction plan have been validated for the target system.

## Scope

Implemented in Reader Core:

- PPCNT counters
- GET-only FEC histogram
- PDDR page 3 module/DDM snapshot
- Q3400 topology-aware aggregation

Not implemented as Reader Core capabilities:

- PDDR page 9
- PMAOS
- PEMI/SNR
- PORT_STATUS
- scheduler/daemon
- database/HTTP API
- final UI/CSV export

## License and attribution

See [`LICENSE`](LICENSE) and [`docs/UPSTREAM_AND_LICENSE.md`](docs/UPSTREAM_AND_LICENSE.md). The public extension preserves upstream attribution and uses the OpenIB.org BSD license option carried by the public `mstflint` license text for redistribution of the published extension sources.
