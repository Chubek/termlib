/* ts_stdlib.h -- the Termscript standard library (hybrid C + .tsc).
 *
 * Each library under this directory is two halves:
 *
 * - a native module (std.<name>, declared in <name>.h, implemented in
 *   <name>.c against termscript.h plus common/ts_std_common.h) that
 *   owns storage, parsing, syscalls and handles; and
 * - a Termscript companion (<name>.tsc) loaded with G:import that
 *   binds the module once and spells shared idioms as plain
 *   Termscript (aliases, defaults, load-or-die).
 *
 * Embedding: link libtermscript_stdlib, call
 * ts_stdlib_register_all() once per VM, and point the VM at the
 * installed companions with ts_stdlib_search_path() (or
 * TERMSCRIPT_PATH).  The Termlib driver and DT_TermVM do all three
 * automatically.
 */
#ifndef TS_STDLIB_H
#define TS_STDLIB_H

#include <termscript/termscript.h>

#include "array/array.h"
#include "autocomp/autocomp.h"
#include "baudrate/baudrate.h"
#include "bidi/bidi.h"
#include "buffer/buffer.h"
#include "cell/cell.h"
#include "codec/codec.h"
#include "color/color.h"
#include "compositor/compositor.h"
#include "css/css.h"
#include "csv/csv.h"
#include "draw/draw.h"
#include "drm/drm.h"
#include "edit_history/edit_history.h"
#include "emulator/emulator.h"
#include "env/env.h"
#include "exec/exec.h"
#include "expect/expect.h"
#include "font/font.h"
#include "font_discovery/font_discovery.h"
#include "fsm/fsm.h"
#include "glob/glob.h"
#include "html/html.h"
#include "http/http.h"
#include "io/io.h"
#include "ipc/ipc.h"
#include "json/json.h"
#include "key/key.h"
#include "layout/layout.h"
#include "list/list.h"
#include "log/log.h"
#include "map/map.h"
#include "menu/menu.h"
#include "notify/notify.h"
#include "panel/panel.h"
#include "pipe/pipe.h"
#include "poll/poll.h"
#include "pty/pty.h"
#include "prompt/prompt.h"
#include "prompt_string/prompt_string.h"
#include "readline/readline.h"
#include "regex/regex.h"
#include "rules/rules.h"
#include "sched/sched.h"
#include "screen/screen.h"
#include "shell/shell.h"
#include "signal/signal.h"
#include "socket/socket.h"
#include "stream/stream.h"
#include "style/style.h"
#include "stylesheet/stylesheet.h"
#include "syntax/syntax.h"
#include "table/table.h"
#include "termcap/termcap.h"
#include "termart/termart.h"
#include "terminfo/terminfo.h"
#include "terminal/terminal.h"
#include "termios/termios.h"
#include "test/test.h"
#include "text_search/text_search.h"
#include "theme/theme.h"
#include "toml/toml.h"
#include "tui/tui.h"
#include "utf8/utf8.h"
#include "verbatim/verbatim.h"
#include "vterm/vterm.h"
#include "widget/widget.h"
#include "word_motion/word_motion.h"
#include "x11/x11.h"
#include "yaml/yaml.h"
#include "gapbuffer/gapbuffer.h"
#include "piecetable/piecetable.h"
#include "memory/memory.h"
#include "persian_script/persian_script.h"
#include "set/set.h"
#include "stdio/stdio.h"
#include "datagram/datagram.h"
#include "procfs/procfs.h"
#include "sysfs/sysfs.h"
#include "crdt/crdt.h"
#include "input/input.h"
#include "font_shape/font_shape.h"
#include "shm/shm.h"
#include "mqueue/mqueue.h"
#include "grammar/grammar.h"
#include "peg/peg.h"
#include "evdev/evdev.h"
#include "uvdev/uvdev.h"
#include "wayland/wayland.h"
#include "index/index.h"
#include "multiplex/multiplex.h"
#include "protocols/protocols.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* Number of bundled native modules. */
#define TS_STDLIB_NMODULES 92
/* Metadata is borrowed immutable static storage; out-of-range returns NULL. */
size_t ts_stdlib_module_count (void);
const TS_Module *ts_stdlib_module_at (size_t index);

/* Register every std.* module on `vm` (duplicate registration fails
 * with TS_ERR_INVAL, like any other module). */
TS_Status ts_stdlib_register_all (TS_VM *vm, TS_Error *error);

/* Install-tree directory holding the .tsc companions
 * (TERMSCRIPT_STDLIB_DIR baked in at build time, "./stdlib"
 * fallback for build-tree runs without install). */
const char *ts_stdlib_default_dir (void);

/* Convenience: ts_vm_add_search_path(vm, ts_stdlib_default_dir()).
 * Best-effort (returns the add status, usually TS_OK). */
TS_Status ts_stdlib_search_path (TS_VM *vm, TS_Error *error);

#ifdef __cplusplus
}
#endif

#endif /* TS_STDLIB_H */
