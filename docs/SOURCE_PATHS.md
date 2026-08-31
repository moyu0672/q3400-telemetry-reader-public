# Source Paths and Upstream Stack

Validated upstream baseline: public `Mellanox/mstflint` tag `v4.35.0-1.9`, commit `8c300f7059b92055325cbbf8e8aff4d44b504e4f`.

```text
q3400_reader
  -> MlxRegLib / RegAccessParser
  -> sendRegister()
  -> maccess_reg
  -> mtcr / MST device access
  -> firmware Access Register
```

Relevant upstream areas are `mlxreg/mlxreg_lib/`, the configured MTCR implementation and `include/mtcr_ul/`, `reg_access/`, `adb_parser/`, and `tools_layouts/`. The validated switch Access Register ADB source path is `tools_layouts/adb/prm/switch/ext/register_access_table.adb`.

The Reader's persistence optimization reuses `mopen()`/device state, `MlxRegLib`, ADB parsing/traversal state, and pre-built request templates. Firmware register semantics and Access Register transport remain upstream behavior.

The runtime ADB used during validation was byte-identical to the matching source-tree switch ADB. This repository intentionally does not redistribute a second runtime ADB copy.
