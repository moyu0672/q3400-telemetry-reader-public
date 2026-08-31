# Register Data Dictionary

## COUNTERS — PPCNT group 0x16

One GET per selected logical channel. Request invariants include `grp=0x16` and `clr=0`.

Materialized values include time since last clear, received bits, symbol errors, corrected bits, lane-0 raw errors, effective errors, and raw/lane-0/effective/symbol BER coefficient+magnitude pairs.

BER is represented as `coefficient x 10^-magnitude`. The sentinel pair `15/255` is preserved rather than collapsed to zero; numerically it is `1.5e-254`.

## HISTOGRAM_READ — PPHCR + PPCNT group 0x23

Two GETs per selected logical channel. PPHCR supplies active histogram type, bin count, and low/high ranges. PPCNT group `0x23` supplies 64-bit occurrence counters. Both accesses are GET-only; PPCNT uses `clr=0`.

At aggregated logical-port level, a consistent bin definition is stored once and occurrences remain ordered by logical channel. Incompatible definitions are marked inconsistent instead of silently merged.

## MODULE_SNAPSHOT — PDDR page 3

One planned GET per selected physical label port, with controlled context fallback for access/context failures only.

When available in the active ADB/response, the typed snapshot includes identity, OUI, manufacturing date, module/cable type and technology, firmware/memory-map data, wavelength, module state/error, power/DDM information, temperature/voltage and thresholds, optical power/bias thresholds, CDR fields, and per-media-lane datapath/Rx-output-valid/power/bias data.

Optional fields remain null/unavailable if the active ADB lacks them. Manufacturing date supports nested ADB fields such as `date_code.hi` / `date_code.lo`. A legitimate boolean `false` for Rx Output Valid is distinct from field absence/capability.

## Aggregated model

`objects[]` is the per-channel raw result set, `modules[]` contains one module result per physical label port, and `aggregated_ports[]` is a convenience view. Aggregation must not fabricate channels, ports, lanes, counters, or module fields.
