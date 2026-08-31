# Upstream, Attribution and License

This project is built against public NVIDIA/Mellanox `mstflint`: validated tag `v4.35.0-1.9`, commit `8c300f7059b92055325cbbf8e8aff4d44b504e4f`.

NVIDIA, Mellanox, Q3400, NVOS, MFT, and related names identify upstream software and the tested platform only. This repository is not an official NVIDIA product, release, support package, or compatibility statement.

The upstream source is distributed under a choice of GPLv2 or the OpenIB.org BSD terms included by the project. This extension uses the OpenIB.org BSD option carried in the copied upstream `LICENSE`; original notices, conditions, and disclaimer are retained. No compiled Reader binary or runtime ADB copy is published here.

Good upstream PR candidates should be small and generic rather than the entire Q3400 Reader. Candidates identified during this work are: preserving legitimate false Rx Output Valid values; nested ADB traversal for `date_code.hi/.lo`; soft handling of optional PDDR3 fields; avoiding redundant context retries after deterministic schema failures; and, separately, discussing a reusable persistent Access Register API/benchmark.

An upstream PR should exclude Q3400 topology tables, device/customer logs, serial numbers, and unrelated aggregation/export code.
