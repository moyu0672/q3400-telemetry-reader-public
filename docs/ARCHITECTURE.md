# Architecture

The Reader is a read-only telemetry layer built on the public `mstflint` Access Register stack. Its main architectural change is not a new register protocol: it keeps MST/device and ADB/parser contexts alive across many GET operations instead of starting a new CLI process for each read.

```text
caller -> Mapping::select() -> Reader
       -> persistent DeviceContext per PCI function
       -> mopen + MlxRegLib + parsed ADB/request templates
       -> sendRegister(..., MACCESS_REG_METHOD_GET, ...)
       -> raw channel/module results -> topology-aware aggregation -> JSON
```

`COUNTERS` reads PPCNT group `0x16` with `clr=0`. `HISTOGRAM_READ` reads PPHCR for the active histogram definition and then PPCNT group `0x23` with `clr=0`. `MODULE_SNAPSHOT` reads PDDR page 3. No Access Register SET path, counter/histogram clear, reset, reboot, or configuration change is part of Reader Core.

The raw layer keeps one object per selected logical channel and one module result per selected physical label port. The aggregation layer groups these into physical/logical front-panel ports without discarding per-channel values. For the validated topology model, channels 1-4 belong to `<label>/1` and channels 5-8 to `<label>/2`; missing entries are never synthesized.

PDDR page 3 is planned once per physical label port. Candidate contexts are sorted deterministically. Fallback is reserved for access/context failures; deterministic schema/decode failures do not trigger retries through all equivalent contexts, and `present=false` is a valid final result. Optional fields remain unavailable/null when absent from the active ADB. Capability bits and actual boolean status values are kept distinct.

A scan can be partially successful. Per-object failures remain in the structured result; exit code 2 represents partial read results, while setup/argument/schema initialization failures use exit code 1. This is especially relevant for FEC histograms because inactive/no-module ports may legitimately have no active histogram.

The complete physical topology is data, not an algorithm. This public repository therefore does not ship the full device-derived mapping used in validation. A target deployment must supply a mapping and PCI-function mapping validated for its own chassis/firmware/host enumeration.
