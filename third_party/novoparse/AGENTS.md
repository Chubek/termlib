# NovoParse

NovoParse is a profile-driven GLR parser generator backed by
`third_party/libglr`. It supports disambiguation, grammar directives, and GRL
rewriters. Its D frontend compiles `.ng` grammars to a versioned S-expression
IR, then renders target source using S-expression profiles in `profiles/`.

- `source/` contains the D reader, compiler, IR, profile loader, emitter, and CLI.
- `runtime/NovoParse.c` implements the native runtime; `runtime/` also contains
  its C/C++, D, and Python APIs/adapters.
- Implemented profiles are C, C++, D, and Python. Other pre-created files are
  placeholders for future targets.
- Keep built-in profile dependencies offline and preserve deterministic output.
- Preserve the vendored `libglr` implementation; dependency additions must be
  pinned and documented in `third_party/DEPENDENCIES.md`.
- Build with `cmake -S . -B build` and `cmake --build build --parallel`.
- Run `ctest --test-dir build --output-on-failure` for end-to-end coverage.
  `dub test` runs the frontend's unit tests.
- Keep grammar, profile, IR, and runtime changes aligned with `docs/` and
  exercise profile changes with a generated parser.
