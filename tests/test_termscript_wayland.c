#define _GNU_SOURCE
#include "termscript/stdlib/ts_stdlib.h"
#include "termscript/stdlib/common/ts_std_common.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-server.h>
static TS_Error error;
#define CHECK(x) do { if (!(x)) { fprintf (stderr, "%d: %s (%s)\n", __LINE__, #x, error.message); exit (1); } } while (0)
static TS_Value integer (int64_t i) { TS_Value v = { .type = TS_INT, .as.integer = i }; return v; }
static TS_Status call (const char *name, TS_Value *r, size_t n, TS_Value *a)
{
  ts_value_free (r); ts_error_clear (&error);
  const TS_FuncDef *fn = ts_module_find_func (&ts_std_wayland_module, name); CHECK (fn);
  return fn->func (NULL, fn->userdata, a, n, r, &error);
}
static const struct wl_interface fixture = { "termscript_fixture", 1, 0, NULL, 0, NULL };
static void bind_fixture (struct wl_client *client, void *data, uint32_t version, uint32_t id)
{ (void)client; (void)data; (void)version; (void)id; CHECK (false); }
static void read_ready (TS_Value *connection, TS_Value *r, int fd)
{
  CHECK (call ("prepare_read", r, 1, connection) == TS_OK);
  struct pollfd p = { fd, POLLIN, 0 };
  CHECK (poll (&p, 1, 1000) == 1 && (p.revents & (POLLIN | POLLHUP)));
  CHECK (call ("read_events", r, 1, connection) == TS_OK);
}
int main (void)
{
  struct wl_display *server = wl_display_create (); CHECK (server);
  struct wl_global *global = wl_global_create (server, &fixture, 1, NULL, bind_fixture); CHECK (global);
  int sockets[2]; CHECK (!socketpair (AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, sockets));
  struct wl_client *client = wl_client_create (server, sockets[0]); CHECK (client);
  TS_Value connection = {0}, r = {0}, fd = integer (sockets[1]);
  CHECK (call ("available", &r, 0, NULL) == TS_OK && r.as.boolean);
  CHECK (call ("connect_fd", &connection, 1, &fd) == TS_OK);
  CHECK (call ("fd", &r, 1, &connection) == TS_OK && r.as.integer != sockets[1]);
  int owned = (int)r.as.integer; CHECK (fcntl (owned, F_GETFD) & FD_CLOEXEC);
  CHECK (call ("read_events", &r, 1, &connection) == TS_ERR_INVAL);
  CHECK (call ("prepare_read", &r, 1, &connection) == TS_OK);
  CHECK (call ("prepare_read", &r, 1, &connection) == TS_ERR_INVAL);
  CHECK (call ("dispatch_pending", &r, 1, &connection) == TS_ERR_INVAL);
  CHECK (call ("cancel_read", &r, 1, &connection) == TS_OK);
  CHECK (call ("cancel_read", &r, 1, &connection) == TS_OK);
  CHECK (call ("globals", &r, 1, &connection) == TS_OK);
  CHECK (!ts_std_list_len (ts_std_list_unwrap (&r, &error, "globals")));
  CHECK (call ("flush", &r, 1, &connection) == TS_OK);
  CHECK (wl_event_loop_dispatch (wl_display_get_event_loop (server), 0) >= 0);
  wl_display_flush_clients (server);
  read_ready (&connection, &r, owned);
  CHECK (call ("prepare_read", &r, 1, &connection) == TS_ERR_WOULD_BLOCK);
  CHECK (call ("dispatch_pending", &r, 1, &connection) == TS_OK);
  CHECK (call ("globals", &r, 1, &connection) == TS_OK);
  ts_std_list_t *globals = ts_std_list_unwrap (&r, &error, "globals"); CHECK (globals && ts_std_list_len (globals) == 1);
  ts_dom_t *item = ts_dom_unwrap (ts_std_list_get (globals, 0), &error, "global"); CHECK (item);
  TS_Value name = {0}; CHECK (ts_dom_to_value (ts_dom_key (item, "interface"), &name, &error) == TS_OK);
  CHECK (!strcmp (name.as.string, fixture.name)); ts_value_free (&name);
  TS_Value snapshot = {0}; CHECK (ts_value_copy (&snapshot, &r) == TS_OK);
  wl_global_destroy (global); wl_display_flush_clients (server);
  read_ready (&connection, &r, owned);
  CHECK (call ("dispatch_pending", &r, 1, &connection) == TS_OK);
  CHECK (call ("globals", &r, 1, &connection) == TS_OK);
  CHECK (!ts_std_list_len (ts_std_list_unwrap (&r, &error, "globals")));
  CHECK (ts_std_list_len (ts_std_list_unwrap (&snapshot, &error, "snapshot")) == 1);
  wl_client_destroy (client);
  CHECK (call ("prepare_read", &r, 1, &connection) == TS_OK);
  struct pollfd p = { owned, POLLIN, 0 }; CHECK (poll (&p, 1, 1000) == 1);
  CHECK (call ("read_events", &r, 1, &connection) == TS_ERR_EOF);
  CHECK (call ("close", &r, 1, &connection) == TS_OK);
  CHECK (call ("close", &r, 1, &connection) == TS_OK);
  CHECK (fcntl (owned, F_GETFD) < 0 && errno == EBADF);
  CHECK (fcntl (sockets[1], F_GETFL) & O_NONBLOCK); CHECK (!close (sockets[1]));
  ts_value_free (&connection); ts_value_free (&snapshot);
  wl_display_destroy (server);
  // Close must cancel an outstanding read intention even with an idle peer.
  CHECK (!socketpair (AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets)); fd = integer (sockets[1]);
  CHECK (call ("connect_fd", &connection, 1, &fd) == TS_OK);
  CHECK (call ("prepare_read", &r, 1, &connection) == TS_OK);
  CHECK (call ("close", &r, 1, &connection) == TS_OK); ts_value_free (&connection);
  const char *old = getenv ("WAYLAND_SOCKET"); char *saved = old ? strdup (old) : NULL;
  int stdin_flags = fcntl (STDIN_FILENO, F_GETFD);
  CHECK (!old || saved); CHECK (!setenv ("WAYLAND_SOCKET", "0", 1));
  CHECK (call ("connect", &connection, 0, NULL) == TS_ERR_INVAL);
  CHECK (fcntl (STDIN_FILENO, F_GETFD) == stdin_flags);
  CHECK (saved ? !setenv ("WAYLAND_SOCKET", saved, 1) : !unsetenv ("WAYLAND_SOCKET")); free (saved);
  CHECK (!close (sockets[0]) && !close (sockets[1])); ts_value_free (&r);
  CHECK (!socketpair (AF_UNIX, SOCK_STREAM, 0, sockets)); fd = integer (sockets[1]);
  CHECK (call ("connect_fd", &connection, 1, &fd) == TS_ERR_INVAL);
  CHECK (!(fcntl (sockets[1], F_GETFL) & O_NONBLOCK));
  CHECK (!close (sockets[0]) && !close (sockets[1]));
  puts ("Wayland local registry fixture OK"); return 0;
}
