# Upstream issue draft: persistent / batched Access Register reads

Target repository: `Mellanox/mstflint`

Suggested title:

`Proposal: persistent / batched Access Register reads for high-frequency telemetry`

## Summary

I would like to propose a generic persistent / batched Access Register read path for high-frequency telemetry workloads.

The motivation is not to replace existing one-shot CLI behavior. The goal is to avoid repeatedly paying fixed userspace initialization costs when many GET-only register reads are required from the same device context.

## Observation

Using the public mstflint `v4.35.0-1.9` baseline, I built a small read-only prototype around the existing Access Register stack and kept the parser/device contexts alive across repeated reads.

In one validated NVIDIA Q3400/NVOS environment, the following were observed:

- repeated persistent PPCNT GET: ~5.15 ms / GET
- one persistent worker: ~194 GET/s
- four independent device contexts: ~466.9 GET/s aggregate
- 580-channel counter hot scan: ~1.286 s
- 580 counters + 73 module/status reads: ~1.754 s hot
- full counters + module + histogram hot scan: ~2.858 s

For comparison, a one-shot `mlxreg`-style invocation in the same development environment was on the order of ~0.56 s per GET because each invocation also paid process startup, ADB parsing, MST/device initialization, and parser setup.

This does **not** imply that the underlying hardware register transaction itself became tens or hundreds of times faster. The improvement comes mainly from reusing initialized userspace state.

## Proposed direction

Would the maintainers consider a reusable API or execution mode that supports one or both of the following?

1. Keep an initialized Access Register/device/parser context alive for repeated GET operations.
2. Accept a batch of GET-only register requests and execute them without reinitializing the full stack for every request.

For devices exposing multiple independent PCI functions / access contexts, a caller could also parallelize independent reads while keeping ordering and error handling explicit.

I think the most useful upstream abstraction would be device-agnostic and independent of Q3400-specific topology or telemetry formatting.

## Safety / scope

The prototype is intentionally GET-only for telemetry. It does not require SET, clear, reset, reboot, or configuration operations.

## Reference implementation and measurements

A public engineering prototype, isolated probes, architecture notes, and benchmark details are available here:

https://github.com/moyu0672/q3400-telemetry-reader-public

Benchmark details:

https://github.com/moyu0672/q3400-telemetry-reader-public/blob/main/docs/BENCHMARK_RESULTS.md

The repository intentionally does not publish device-derived topology, runtime ADB copies, raw device logs, or hardware-specific serial information.

If this direction is useful to mstflint, I would be interested in guidance on which layer would be the preferred place for a minimal generic implementation: `MlxRegLib`, a lower Access Register abstraction, or a CLI-level batch/persistent mode.
