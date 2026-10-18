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
command runs. `check` also validates all selected source files, source kinds,
Termscript backend selection and install inputs before build hooks run. Source
symlinks resolving outside the manifest root are rejected. C11/C++20 objects are compiled with PIC, archives are recreated,
and executable dependencies link in reverse topological order. Header changes
under the project root conservatively invalidate objects. Objects cache command
signatures; archive creation and final linking currently run every build.
Compilation, archiving and executable linking publish outputs atomically after success:
failed commands leave the previous object/archive/executable intact and remove their
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

## External CMake projects

`cmake_project` is an explicit opt-in bridge for **trusted** CMake sources:

```ruby
cmake_project :value, source_dir: 'vendor/value', target: 'value',
              artifact: 'libvalue.a', options: ['-DBUILD_TESTING=OFF'],
              component: 'development', cxx: false
```

`check` validates the nonempty source `CMakeLists.txt` and target/artifact/options
shape without configuring or downloading anything. `build` configures and builds
the requested target, then requires the declared artifact to exist inside the
build tree. Artifacts use normalized relative paths; options are explicit `-D`
cache definitions, not arbitrary CMake command-line operations. CMake sources
may be outside the manifest root because this is an **explicit external project**,
not a bypass for the native source policy. `CMAKE` selects the executable;
`CMAKE_BUILD_PARALLEL_LEVEL` sets parallelism. Compiler environment settings follow
CMake's first-configuration/cache semantics. Use a fresh build directory or
explicit cache definitions when changing cached compiler configuration.

By default the build cache is `build/cmake/TARGET_NAME`; optional `cache: 'name'`
lets compatible recipes share one tree. Do not concurrently reconfigure a shared
cache from separate Termak processes. `kind: :executable` labels executable
artifacts; `cxx: true` selects the C++ linker for native dependents. CMake dependency
artifacts are not implicitly injected into an external project, and its external
link-library closure is not guessed for native consumers.

Installation reconfigures the requested prefix, rebuilds, then invokes CMake's
installer, forwarding an explicit DESTDIR (or clearing inherited DESTDIR when
none was requested). Component installs build the selected target; full installs
build all ordinary project targets first. CMake publishes artifacts and performs
delegated installs according to **its own** rules, not Termak's atomic compiler/
archive publication or per-file symlink guards. Prefix traversal and symlinked
stage/prefix ancestors are rejected before delegation, but trusted CMake install
scripts can perform arbitrary operations. Neither Ruby nor CMake is a sandbox.

## Standard library recipes

Every shipped module directory has a functional Termakfile, including aliases
and `common`, `gen`, `q` infrastructure. `Stdlib.rb` recipes use the authoritative
CMake module list and dependency/feature settings rather than duplicating dozens
of third-party include paths. These recipes require a source checkout and the
same offline vendored/platform build prerequisites as CMake.

From the repository root:

```sh
# Full stdlib/runtime build and installation, including native dependency closure.
termak -f termscript/stdlib/Termakfile -B build/termak build
termak -f termscript/stdlib/Termakfile -B build/termak --prefix /usr/local --destdir stage install

# A real individual adapter archive, plus its header/companion component install.
termak -f termscript/stdlib/peg/Termakfile -B build/termak build
termak -f termscript/stdlib/peg/Termakfile -B build/termak --prefix /usr/local --destdir stage install
```

Per-module output is `build/termak/cmake/termscript/termscript/stdlib/libtermscript_std_NAME.a`.
It contains real adapter objects and **requires the full stdlib/runtime closure**
when linked; it is not a self-contained plugin or new native ABI. The first
module build also builds that shared closure. Alias archives compile their real
provider (`font`/`css`), not empty source files. `common` builds shared helpers;
`gen` and `q` legitimately install headers/generator inputs without fake binaries.
The CRDT manifest retains its independent core target and additionally builds
the VM adapter. Module component installation assumes the base runtime/stdlib is
already installed; use the top-level stdlib manifest for a complete installation.
Adapter archives are excluded from normal CMake builds/default installs and can
also be selected through `termscript_std_NAME` CMake targets and
`termscript-module-NAME` install components.

The header-only `termscript/include/Termakfile` installs the implemented C/C++
headers, including `TSC-Protocol.h`. `termscript/termak/Termakfile` builds/installs
the real launcher and Ruby implementation. `index`, `multiplex` and `protocols`
now have real module recipes; see `stdlib/infrastructure-modules.md` for their
implemented scopes and security limitations.

`plugin 'path.rb'` evaluates a local project plugin. `on(:before_build)` and
`on(:after_build)` register hooks receiving the project. A hook failure aborts
the build; it is not silently swallowed.

`PackageRepository` reads an explicit local JSON index keyed by package and
version, with `url` and `sha256` fields. `fetch(name, version, cache:)` uses a
verified cache or explicitly downloads via HTTPS with peer verification,
timeouts and a 64 MiB limit. Redirects and credential-bearing URLs are rejected.
Nothing downloads during ordinary builds, and archives are never implicitly
extracted or executed. Online downloads are not covered by the offline tests.

Fetch an explicitly pinned package without loading a Termakfile:

```sh
termak --index packages.json --cache .termak/cache fetch package_name 1.0
```

This prints the verified archive path. It is a pinned archive fetcher, not a
version solver: dependency resolution, archive extraction and package execution
are not implicit. `--prefix` must be absolute and cannot contain `..`, including
when `--destdir` is used. Manifests/plugins remain trusted Ruby; path checks are
not a sandbox against malicious Ruby or concurrent filesystem changes.
