/* Numeric baud rates are distinct from platform-specific speed_t constants. */
#include "baudrate/baudrate.h"
#include "common/tty.h"
#include <errno.h>

typedef struct { int64_t baud; speed_t speed; } baud_entry;
static const baud_entry rates[] = {
  {0, B0}, {50, B50}, {75, B75}, {110, B110}, {134, B134}, {150, B150},
  {200, B200}, {300, B300}, {600, B600}, {1200, B1200}, {1800, B1800},
  {2400, B2400}, {4800, B4800}, {9600, B9600}, {19200, B19200}, {38400, B38400},
#ifdef B57600
  {57600, B57600},
#endif
#ifdef B115200
  {115200, B115200},
#endif
#ifdef B230400
  {230400, B230400},
#endif
#ifdef B460800
  {460800, B460800},
#endif
#ifdef B500000
  {500000, B500000},
#endif
#ifdef B576000
  {576000, B576000},
#endif
#ifdef B921600
  {921600, B921600},
#endif
#ifdef B1000000
  {1000000, B1000000},
#endif
#ifdef B1152000
  {1152000, B1152000},
#endif
#ifdef B1500000
  {1500000, B1500000},
#endif
#ifdef B2000000
  {2000000, B2000000},
#endif
#ifdef B2500000
  {2500000, B2500000},
#endif
#ifdef B3000000
  {3000000, B3000000},
#endif
#ifdef B3500000
  {3500000, B3500000},
#endif
#ifdef B4000000
  {4000000, B4000000},
#endif
};

bool
ts_baud_encode (int64_t baud, speed_t *speed)
{
  size_t i;
  for (i = 0; i < sizeof rates / sizeof rates[0]; i++)
    if (rates[i].baud == baud) { *speed = rates[i].speed; return true; }
  return false;
}

int64_t
ts_baud_decode (speed_t speed)
{
  size_t i;
  for (i = 0; i < sizeof rates / sizeof rates[0]; i++)
    if (rates[i].speed == speed) return rates[i].baud;
  return -1;
}

static TS_Status
b_supported (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  int64_t baud;
  speed_t speed;
  (void) ud;
  if (ts_std_argc (vm, n, 1, 1, e, "supported") || ts_std_int (a, &baud, e, "supported")) return TS_ERR_INVAL;
  ts_std_ret_bool (ret, ts_baud_encode (baud, &speed));
  return TS_OK;
}

static TS_Status
b_list (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  ts_std_list_t *l;
  size_t i;
  (void) ud; (void) a;
  if (ts_std_argc (vm, n, 0, 0, e, "rates")) return TS_ERR_INVAL;
  l = ts_std_list_new ();
  if (!l) return ts_std_oom (e);
  for (i = 0; i < sizeof rates / sizeof rates[0]; i++)
    if (ts_std_list_push_int (l, rates[i].baud)) { ts_std_list_free (l); return ts_std_oom (e); }
  return ts_std_list_wrap (ret, l, e);
}

static TS_Status
b_speed (TS_VM *vm, void *ud, const TS_Value *a, size_t n, TS_Value *ret, TS_Error *e)
{
  int64_t fd, input, output;
  struct termios t;
  speed_t ispeed, ospeed;
  if (ts_std_argc (vm, n, ud ? 2 : 1, ud ? 3 : 2, e, "baudrate") || ts_std_int (a, &fd, e, "baudrate")) return TS_ERR_INVAL;
  if (fd < 0 || fd > INT32_MAX) return ts_std_inval (e, "baudrate: invalid fd");
  if (tcgetattr ((int) fd, &t)) return ts_std_system (e, "tcgetattr");
  if (!ud)
    {
      bool in = n == 2 && a[1].type == TS_BOOL && a[1].as.boolean;
      if (n == 2 && a[1].type != TS_BOOL) return ts_std_inval (e, "get: bool input selector required");
      input = ts_baud_decode (in ? cfgetispeed (&t) : cfgetospeed (&t));
      if (input < 0) ts_std_ret_nil (ret); else ts_std_ret_int (ret, input);
      return TS_OK;
    }
  if (ts_std_int (a + 1, &output, e, "set")) return TS_ERR_INVAL;
  input = output;
  if (n == 3 && ts_std_int (a + 2, &input, e, "set")) return TS_ERR_INVAL;
  if (!ts_baud_encode (input, &ispeed) || !ts_baud_encode (output, &ospeed))
    return ts_std_inval (e, "set: unsupported baud rate");
  if (cfsetospeed (&t, ospeed) || cfsetispeed (&t, ispeed)) return ts_std_system (e, "cfsetspeed");
  while (tcsetattr ((int) fd, TCSANOW, &t))
    { if (errno == EINTR) continue; return ts_std_system (e, "tcsetattr"); }
  ts_std_ret_bool (ret, true);
  return TS_OK;
}

static const TS_FuncDef funcs[] = {
  {"supported", b_supported, NULL}, {"rates", b_list, NULL},
  {"get", b_speed, NULL}, {"set", b_speed, (void *) 1}, {NULL, NULL, NULL}
};
const TS_Module ts_std_baudrate_module = {"std.baudrate", funcs};
