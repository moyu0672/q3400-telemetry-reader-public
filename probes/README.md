# Validation probes

These programs are intentionally separate from `q3400_reader`. They are small experimental tools used to isolate one Access Register path at a time when validating performance or decoding behavior against a Q3400 environment.

The probe sources are GET-only. They are not required for normal Reader use and are kept outside the core source directory to make the production Reader boundary explicit.

- `ppcnt_persistent_bench.cpp`: persistent PPCNT group `0x16` latency/throughput measurement.
- `fec_histogram_probe.cpp`: one-shot PPHCR + PPCNT group `0x23` FEC histogram validation.
- `pddr_module_probe.cpp`: one-shot PDDR page 3 module/DDM validation.

Hardware use still requires a validated ADB, PCI function, local-port mapping, and target software/firmware environment.
