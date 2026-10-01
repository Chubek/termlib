# Termlib Guide

## Build and run

```sh
cmake -S . -B build
cmake --build build
./build/bin/termscript example.ts
```

Use `termscript -c -o example.c example.ts` to emit C without compiling it. The generated unit links against the Termscript runtime and any native modules it calls.

## C usage

Include `termlib.h`, initialize option structures, pass a `DT_Error` when diagnostics are useful, and close every returned handle. Treat `DT_ERR_EOF`, `DT_ERR_TIMEOUT`, and `DT_ERR_WOULD_BLOCK` as normal control-flow outcomes.

## C++ usage

Include `termlib.hpp`. Factory functions return `std::unique_ptr` wrappers; wrappers are movable and release their C handles automatically.

## Termscript

Termscript uses `G:load`, `G:puts`, and native module calls. Companion `.tsc` modules are resolved at compile time when imports use constant names. Native modules remain linked and are resolved through the runtime registry.
