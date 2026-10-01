#ifndef TS_STD_TTY_H
#define TS_STD_TTY_H
#include "common/ts_std_common.h"
#include <termios.h>

typedef struct {
  int fd;
  struct termios original, current;
  bool dirty;
} ts_tty_state_t;
/* fd is borrowed; state owns a close-on-exec duplicate. */
TS_Status ts_tty_save (ts_tty_state_t *s, int fd, TS_Error *e);
TS_Status ts_tty_restore (ts_tty_state_t *s, TS_Error *e);
void ts_tty_dispose (ts_tty_state_t *s);
void ts_tty_raw (struct termios *t);
bool ts_baud_encode (int64_t baud, speed_t *speed);
int64_t ts_baud_decode (speed_t speed);
int ts_dup_cloexec (int fd);
int ts_poll_fd (int fd, short events, int cancel_fd, int timeout);
#endif
