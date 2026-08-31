# Q3400 Topology Mapping

The Reader separates topology validation from telemetry acquisition. Its TSV mapping requires:

```text
label_port	ipil	channel	split	pciconf	local_port	module	sub_module
```

`channel` is the mapping-local channel 1-4. The derived logical channel is:

```text
logical_channel = (ipil - 1) * 4 + mapping_channel
```

Thus `ipil=1` produces CH1-CH4 and `ipil=2` produces CH5-CH8. In the validated model CH1-CH4 group into `<label>/1` and CH5-CH8 into `<label>/2`. Only entries actually present in the mapping are used; missing groups are not synthesized.

The prototype contains a tested `pciconf0..3 -> PCI BDF` table. PCI enumeration is environment-specific and must be validated or adapted before hardware access.

Before `--read`, verify unique `(label_port, logical_channel)` identities, intended ipil/channel grouping, each PCI function, firmware local-port numbering, module/sub-module metadata, and that the mapping belongs to the target chassis/firmware topology.

The complete device-derived 580-object mapping from the private validation environment is intentionally not published here and should not be treated as a universal Q3400 constant.
