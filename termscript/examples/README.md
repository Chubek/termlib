# Examples

- **Runnable:** `MyApp.ta` builds a plain block-letter logo; `Welcome.ta` builds
  a centered, framed truecolor welcome card using [`std.termart`](../stdlib/termart/README.md).
  Run either with `termscript path/to/sample.ta`.

- **Runnable:** `core.tsc` exercises the currently implemented function,
  expression, import and entry-point subset.
- **Runnable:** [`../../xw3m/`](../../xw3m/) contains a terminal browser prototype
  using raw strings and read indexing. It is not a claim of full application
  example compatibility or superiority to w3m.
- **Runnable:** `crdt.tsc` demonstrates concurrent native RGA editing,
  out-of-order delta delivery, convergence and snapshot recovery, without a
  network or external CRDT dependency.
- **Illustrative:** `coedit.tsc`, `flashbench.tsc`, `kmsterm.tsc`, `tmx.tsc`,
  and `tsh.tsc` describe the target application language and APIs. They are
  not yet runnable and were not rewritten to hide unsupported features.

See [`../IMPLEMENTATION.md`](../IMPLEMENTATION.md) for the support boundary and
the shared VM/C/QBE/Jiterati regression programs under `tests/`.
