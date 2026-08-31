# Build and Integration

This repository is an extension source package for a matching public `mstflint` tree, not a standalone replacement for `mstflint`.

Start from `Mellanox/mstflint` tag `v4.35.0-1.9` (validated commit `8c300f7059b92055325cbbf8e8aff4d44b504e4f`) and follow upstream bootstrap/configure prerequisites.

The Reader integration adds `mlxreg/q3400_reader.cpp` and `mlxreg/pddr_module_snapshot.h`. A minimal Automake target is:

```make
noinst_PROGRAMS += q3400_reader
q3400_reader_SOURCES = q3400_reader.cpp pddr_module_snapshot.h
q3400_reader_DEPENDENCIES = $(mstreg_DEPENDENCIES)
q3400_reader_LDADD = $(q3400_reader_DEPENDENCIES) $(liblzma_LIBS) ${LDL} $(expat_LIBS)
q3400_reader_LDFLAGS = -static
```

Regenerate/reconfigure as required by upstream, then build conservatively, e.g. `make -C mlxreg q3400_reader -j2`.

Run `--self-test-aggregation` before hardware access. Then validate the topology mapping without `--read`. Only after validating PCI functions, mapping, target MFT/NVOS compatibility, and ADB should the GET-only hardware path be used.

The temporary CLI accepts `counters`, `histogram`, and `module` capabilities. Exit code 0 means no requested read failures, 1 means fatal setup/argument/schema initialization error, and 2 means a partial result whose JSON still contains successful objects and per-object errors.
