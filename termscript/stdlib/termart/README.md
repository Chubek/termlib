# std.termart

Terminal art and logo builders. `use termart` loads the native module; the
`termart/termart.tsc` companion is also available through `G:import`.

| Function | Result |
| --- | --- |
| `banner(text, ink = "#")` | Original 5×7 block font, one column between glyphs. ASCII lowercase folds to uppercase. Supports A–Z, digits, space, `- . ! ?` and newline. Ink is one visible ASCII character. |
| `frame(text, padding = 1)` | ASCII border around all lines, padded to the widest display-cell width. Horizontal padding is 0–64; no vertical padding. |
| `center(text, width)` | Each line padded on both sides to at least `width` cells (0–4096). Odd extra space goes on the right; never truncates. |
| `color(text, red, green, blue)` | Explicit ANSI truecolor foreground with a final SGR reset; components are integers 0–255. Apply **after** layout. |

Newlines separate lines; a final newline does not create another empty line.
Frame output has no final newline. Center preserves a final newline. Banner
has seven rows per input line, no final newline; empty input yields `""`.
An empty frame contains one empty content row. Use `print` to display results.

## Contracts and limits

All arguments are borrowed; each successful call returns a fresh VM-owned string.
There are no handles, file descriptors, global mutable state, terminal changes,
or external art/font dependencies. Calls are reentrant; obey the enclosing VM's
thread-safety policy. Partial buffers are freed on failure. No blocking I/O or
cancellation is involved.

Inputs must be valid UTF-8 without C0/C1 controls or DEL (except newline).
Escape sequences, tabs and carriage returns are rejected, not interpreted.
Frame/center use the stdlib's locale-independent Unicode cell-width helpers;
terminal/font-specific widths and bidi rendering can differ. Banner deliberately
rejects unsupported glyphs rather than substituting silently.

Input and output are bounded to 1 MiB; every output line is at most 4096 columns
(including the border). Size-limit errors return `TS_ERR_LIMIT`; invalid types,
characters, widths, padding or colors return `TS_ERR_INVAL`; malformed UTF-8
returns `TS_ERR_PARSE`. Allocation errors
return `TS_ERR_NOMEM`; details are in `TS_Error`. No partial result is published.

Color is opt-in: no TTY/`NO_COLOR` detection. The caller decides whether to use it
for redirected output. The final reset restores terminal defaults, not an earlier
style. Color output cannot be fed back into these functions because escapes are
rejected. Text is not a general-purpose Unicode or terminal security sanitizer.

## Runnable samples

From the repository root after building:

```sh
build/bin/termscript termscript/examples/MyApp.ta
build/bin/termscript termscript/examples/Welcome.ta
```

The `.ta` extension is conventional for these art samples; they are ordinary
Termscript programs. CTest checks both samples and an assertion fixture across
VM/C and the enabled QBE/Jiterati backends, plus native invalid-input, Unicode,
geometry and resource-limit tests.
