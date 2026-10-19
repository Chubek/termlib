# Termlib

Termlib is a portable terminal systems library written in C11. It provides PTY and TTY sessions, Terminfo loading and expansion, session recording and replay, local and remote framed connections, and the Termscript automation language.

## What is included

- **`libtermlib`** — TTY/PTY management, Terminfo, recording, replay, messages, and connections.
- **`libtermscript_standalone`** — the standalone Termscript parser and VM.
- **`libtermscript`** — Termlib bindings for the Termscript VM, including the Termlib C backend.
- **`libtermscript_stdlib`** — native standard modules and `.tsc` companion modules.
- **`termscript`** — command-line interpreter and ahead-of-time C/QBE compiler.
- **`termlib.hpp`** — header-only C++11 RAII wrappers for the C API.

## Quick start

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/bin/termscript termscript/examples/core.tsc
```

Run a script through the VM, or emit a standalone C translation unit:

```sh
termscript script.ts
termscript -c -o script.c script.ts
```

Generated C uses the compiled Termscript runtime; it does not parse the original source at startup. See [GUIDE.md](GUIDE.md) for linkage examples and [INSTALL.md](INSTALL.md) for installation details.

## API families

The public C API is declared in [`termlib.h`](termlib.h). Include [`termlib.hpp`](termlib.hpp) for movable, ownership-safe C++ wrappers. All fallible C calls return `DT_Status` and can fill a `DT_Error`; normal EOF, timeout, cancellation, and would-block outcomes have dedicated status values.

## Documentation

The manual is in [`manual/`](manual/). Start with:

- [GUIDE.md](GUIDE.md) — concepts, examples, and generated-C workflow.
- [INSTALL.md](INSTALL.md) — dependencies, build options, and installation.
- [Termscript Manual](manual/termscript.md) — language, CLI, modules, resources,
  backends, embedding, and native extensions.
- [manual/03-api-contracts.md](manual/03-api-contracts.md) — ownership and lifetime rules.
- [manual/19-c-backend.md](manual/19-c-backend.md) — the Termscript C backend.

## License

See the license files in the repository and in `third_party/` for component-specific terms.
## QBE native compilation

On Linux x86-64, build a native executable with
`termscript --backend qbe -o program script.tsc`. The bundled QBE backend
generates instructions and Keystone assembles them. Use `--backend qbe -c`
to emit QBE IL. See [native compilation](manual/23-native-compilation.md)
for build options, runtime linkage, and embedding APIs.
