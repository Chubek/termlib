# Chapter 20: Standard Library

For the current Termscript module catalogue, import semantics, and runnable
examples, see the [Termscript Manual](termscript.md#6-standard-library) and
[stdlib reference](../termscript/stdlib/README.md).

Companion `.tsc` files are inlined only for constant imports. Native modules are linked and looked up through the registry.

## Scope

This chapter documents the stable Termlib contract and the decisions applications must make when using it.

## Recommended pattern

Initialize option structures with their `*_options_init` function, check every returned status, and release each owned handle with its matching cleanup function. Keep borrowed pointers and file descriptors valid for the lifetime promised by the API.

## Reference

See `termlib.h` for exact signatures and `termlib.hpp` for the header-only C++ facade.
