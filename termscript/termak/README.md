# Termak

`build/bin/termak` launches the Ruby DSL using Ruby on PATH. It uses only Ruby
standard libraries. Termakfiles and plugins are **trusted executable Ruby**, not
a sandbox and not safe to load from untrusted repositories.

Runnable manifest shape (with corresponding `value.c` and `main.c` files):

```ruby
library :value, sources: ['value.c'], include_dirs: ['include']
executable :app, sources: ['main.c'], depends: [:value], ldflags: ['-pthread']
install_file 'include/value.h', 'include/value.h'
```

```sh
termak -f Termakfile -B build check
termak -f Termakfile -B build build app
termak --prefix /usr/local --destdir stage install app
termak list
```

Options precede the subcommand. The default command builds all targets.
Dependencies are validated for missing targets and cycles before any build
command runs. C11/C++20 objects are compiled with PIC, archives are recreated,
and executable dependencies link in reverse topological order. Header changes
under the project root conservatively invalidate objects. Objects cache command
signatures; archive creation and final linking currently run every build.
Compilation and executable linking publish outputs atomically after success:
failed commands leave the previous object/executable intact and remove their
temporary output. C++ sources anywhere in the native dependency graph select
the C++ linker, including for executables whose own sources are all C.
Sources must stay inside the manifest root. This is not a complete dependency
file parser for external headers; clean rebuilds are required for those changes.

An executable with exactly one `.tsc` source invokes the Termscript driver:

```ruby
executable :app, sources: ['app.tsc'], backend: 'qbe'
```

Native `depends` entries on Termscript executables are rejected explicitly;
passing their archives to the Termscript driver's link step is not implemented.

`CC`, `CXX`, `AR`, `CPPFLAGS`, `CFLAGS`, `CXXFLAGS`, `LDFLAGS`, and `TERMSCRIPT`
configure commands/flags. Command strings are split with Shellwords, never
evaluated as shell programs. Build output defaults to `build/` beside the
manifest. Installation copies built archives/executables and declared files
under prefix/DESTDIR; traversal and symlink destinations/ancestors are rejected.
This does not protect against hostile concurrent filesystem modifications.

`plugin 'path.rb'` evaluates a local project plugin. `on(:before_build)` and
`on(:after_build)` register hooks receiving the project. A hook failure aborts
the build; it is not silently swallowed.

`PackageRepository` reads an explicit local JSON index keyed by package and
version, with `url` and `sha256` fields. `fetch(name, version, cache:)` uses a
verified cache or explicitly downloads via HTTPS with peer verification,
timeouts and a 64 MiB limit. Redirects and credential-bearing URLs are rejected.
Nothing downloads during ordinary builds, and archives are never implicitly
extracted or executed. Online downloads are not covered by the offline tests.
