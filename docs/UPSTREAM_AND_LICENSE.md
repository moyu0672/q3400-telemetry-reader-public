# Upstream, Attribution and License

This project is built against public NVIDIA/Mellanox `mstflint`: validated tag `v4.35.0-1.9`, commit `8c300f7059b92055325cbbf8e8aff4d44b504e4f`.

NVIDIA, Mellanox, Q3400, NVOS, MFT, and related names identify upstream software and the tested platform only. This repository is not an official NVIDIA product, release, support package, or compatibility statement.

The upstream source is distributed under a choice of GPLv2 or the OpenIB.org BSD terms included by the project. This extension uses the OpenIB.org BSD option carried in the copied upstream `LICENSE`; original notices, conditions, and disclaimer are retained. No compiled Reader binary or runtime ADB copy is published here.

Upstream contributions should be small, generic, and independently justified rather than submitting the entire Q3400 Reader. The most suitable first feasibility area identified during this work is a hardware-independent regression test around nested ADB uint64 subfields such as `date_code.hi` / `date_code.lo`, but only if current upstream behavior or test coverage demonstrates a real gap. Optional PDDR3 field handling and a reusable persistent Access Register API are broader follow-up topics. Project-specific topology fallback policy and aggregation behavior should remain outside an initial upstream patch unless a generic upstream requirement is demonstrated.

The Reader's previous distinction between an unavailable Rx Output Valid field and a legitimate boolean `false` is useful inside this project, but it is not currently established as an upstream `mlxlink` defect and should not be presented as one without an independent reproducer against current upstream code.

Any upstream PR should exclude Q3400 topology tables, device/customer logs, serial numbers, private runtime identifiers, and unrelated aggregation/export code.
