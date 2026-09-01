# Benchmark Results

Measurements below are from one validated Q3400/NVOS environment using the public `mstflint` 4.35 source baseline. They are engineering observations, not guarantees for other systems.

| Workload | Observed result |
|---|---:|
| Repeated persistent PPCNT GET | ~5.15 ms / GET |
| One persistent worker | ~194 GET/s |
| Four independent device contexts, aggregate | ~466.9 GET/s |
| 580-channel COUNTERS hot scan | ~1.286 s |
| 580 COUNTERS + 73 MODULE hot scan | ~1.754 s |
| 580 COUNTERS + 73 MODULE cold one-shot total | ~4.015 s |
| One physical port, COUNTERS + MODULE + HISTOGRAM | ~63 ms hot |
| Full COUNTERS + MODULE + HISTOGRAM hot scan | ~2.858 s |
| Full COUNTERS + MODULE + HISTOGRAM one-shot total | ~5.464 s |

For the last full-capability run, cold initialization was about 2.606 s, channel scanning about 2.379 s, and module scanning about 0.479 s.

The main gain is removal of repeated fixed overhead. A one-shot `mlxreg`-style command in the same development environment was on the order of 0.56 s per GET because each invocation paid process startup, ADB parsing, MST/device initialization, and parser setup again. The persistent Reader pays those costs once and reuses contexts/templates. This does not mean the underlying hardware register transaction became tens or hundreds of times faster.

`probes/ppcnt_persistent_bench.cpp` is the isolated persistent PPCNT benchmark used to measure the single-context GET path. `probes/fec_histogram_probe.cpp` isolates the two-GET histogram path, and `probes/pddr_module_probe.cpp` isolates module/status register reads. These probes are validation tools rather than Reader Core dependencies.

In one full-chassis histogram run, 112 of 580 requested logical-channel histogram reads succeeded. The same run found 14 present modules; `14 x 8 = 112`. This matched the active/present-port population and was treated as expected partial availability, not as a histogram implementation failure.

For reproducibility, record the upstream commit, target MFT/NVOS version, ADB hash, Reader commit, mapping hash/counts, capabilities, cold/hot distinction, and success/failure counts. Do not publish device-derived topology, serial numbers, raw customer/device logs, or other environment-specific data solely for benchmark reproducibility.
