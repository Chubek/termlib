# Chapter 02: Building

The generated Termscript parser descriptor is produced from
`termscript/Termscript.ng` by the vendored NovoParse generator; CMake makes this
a build dependency.

Termscript-related build options:

- `DOMWEAVE_BUILD_TERMSCRIPT_STDLIB=ON|OFF`: build the hybrid native/.tsc
  standard library.
- `TERMSCRIPT_NATIVE=ON|OFF`: enable QBE/Keystone native executable support on
  Linux x86-64.
- `TERMSCRIPT_JITERATI=ON|OFF`: enable the experimental host-AMD64 Jiterati
  execution backend.

## Driver backend flags

After building, the driver is `build/bin/termscript`:

```sh
build/bin/termscript script.tsc                    # VM
build/bin/termscript -c -o script.c script.tsc     # C output
build/bin/termscript -o program script.tsc         # C backend link
build/bin/termscript --backend qbe -c -o script.ssa script.tsc
build/bin/termscript --backend qbe -o program script.tsc
build/bin/termscript --backend jiterati script.tsc
```

`--cc CC` selects the compiler for link steps.  `--static` requests static
linking for `-o` builds.  `--backend jiterati` cannot be combined with `-c` or
`-o` because it executes directly.

## Scope

This chapter documents the stable Termlib contract and the decisions
applications must make when using it.

## Recommended pattern

Initialize option structures with their `*_options_init` function, check every returned status, and release each owned handle with its matching cleanup function. Keep borrowed pointers and file descriptors valid for the lifetime promised by the API.

## Reference

See `termlib.h` for exact signatures and `termlib.hpp` for the header-only C++ facade.
