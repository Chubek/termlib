#ifndef TS_STD_TERMINAL_INTERNAL_H
#define TS_STD_TERMINAL_INTERNAL_H
#include "common/tty.h"
typedef struct {
  int input, output, cancel[2], original_flags, original_output_flags;
  bool flags_changed;
  ts_tty_state_t tty;
  const char *status;
} ts_terminal_t;
ts_terminal_t *ts_terminal_unwrap (const TS_Value *a, TS_Error *e);
TS_Status ts_terminal_raw (ts_terminal_t *s, TS_Error *e);
TS_Status ts_terminal_write_all (ts_terminal_t *s, const char *data,
                                 size_t len, int timeout, TS_Error *e);
#endif
