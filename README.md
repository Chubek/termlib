# Termlib

Termlib is a portable C library for PTY and TTY management, Terminfo parsing and expansion, terminal recording and replay, transport-neutral connections, and the Termscript automation language.

## Components

- `libtermlib`: PTY, TTY, Terminfo, recording, replay, and connections.
- `libtermscript`: Termlib's Termscript VM and C backend.
- `libtermscript_standalone`: the standalone Termscript runtime.
- `libtermscript_stdlib`: standard native and companion modules.
- `termscript`: command-line VM and ahead-of-time compiler driver.
- `termlib.hpp`: header-only C++ RAII wrapper.

Start with [GUIDE.md](GUIDE.md), [INSTALL.md](INSTALL.md), and the [manual](manual/).
