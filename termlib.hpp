#ifndef TERMLIB_HPP
#define TERMLIB_HPP

/* Official C++ wrapper for Termlib.  This header is header-only; link the
 * same libtermlib/libtermscript libraries as the C API. */
#include "termlib.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace termlib {

using Status = DT_Status;
using Error = DT_Error;

inline const char *status_string(Status s) noexcept { return dt_status_string(s); }
inline void clear(Error &e) noexcept { dt_error_clear(&e); }

struct ErrorState {
  DT_Error value{};
  ErrorState() noexcept { dt_error_clear(&value); }
  void clear() noexcept { dt_error_clear(&value); }
  operator DT_Error *() noexcept { return &value; }
  DT_Error *get() noexcept { return &value; }
  const DT_Error *get() const noexcept { return &value; }
};

inline bool ok(Status s) noexcept { return s == DT_OK; }
inline unsigned int version_number() noexcept { return dt_version_number(); }
inline const char *version_string() noexcept { return dt_version_string(); }
inline bool feature_query(const char *name) noexcept { return dt_feature_query(name); }

class TIDB;
class Profile;
class TIParser;
class TTY;
class PTY;
class Puppeteer;
class Recorder;
class Replayer;
class Message;
class Envelope;
class LocalConnection;
class RemoteConnection;
class TermVM;

template<class T, void (*Free)(T *)> class Handle {
protected:
  T *p_ = nullptr;
  explicit Handle(T *p) noexcept : p_(p) {}
public:
  Handle() noexcept = default;
  ~Handle() { Free(p_); }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  Handle(Handle &&o) noexcept : p_(o.p_) { o.p_ = nullptr; }
  Handle &operator=(Handle &&o) noexcept {
    if (this != &o) { Free(p_); p_ = o.p_; o.p_ = nullptr; }
    return *this;
  }
  explicit operator bool() const noexcept { return p_ != nullptr; }
  T *get() noexcept { return p_; }
  const T *get() const noexcept { return p_; }
  T *release() noexcept { T *q = p_; p_ = nullptr; return q; }
  void reset(T *p = nullptr) noexcept { if (p_ != p) { Free(p_); p_ = p; } }
};

class TIDB : public Handle<DT_TIDB, dt_tidb_close> {
  explicit TIDB(DT_TIDB *p) noexcept : Handle(p) {}
public:
  static std::unique_ptr<TIDB> open(const char *const *paths, std::size_t count, Error *e = nullptr) {
    auto p = dt_tidb_open(paths, count, e); return p ? std::unique_ptr<TIDB>(new TIDB(p)) : nullptr;
  }
  static std::unique_ptr<TIDB> open(const std::vector<std::string> &paths, Error *e = nullptr) {
    std::vector<const char *> p; p.reserve(paths.size()); for (auto &s : paths) p.push_back(s.c_str());
    return open(p.data(), p.size(), e);
  }
  static std::unique_ptr<TIDB> open_default(Error *e = nullptr) {
    auto p = dt_tidb_open_default(e); return p ? std::unique_ptr<TIDB>(new TIDB(p)) : nullptr;
  }
  std::unique_ptr<Profile> load(const char *name, Error *e = nullptr);
  std::unique_ptr<Profile> load(const std::string &name, Error *e = nullptr) { return load(name.c_str(), e); }
  std::unique_ptr<Profile> load_default(Error *e = nullptr);
};

class Profile : public Handle<DT_TIProf, dt_tiprof_free> {
  explicit Profile(DT_TIProf *p) noexcept : Handle(p) {}
  friend class TIDB; friend class TIParser; friend class Puppeteer;
public:
  const char *name() const noexcept { return dt_tiprof_name(p_); }
  std::size_t alias_count() const noexcept { return dt_tiprof_alias_count(p_); }
  const char *alias(std::size_t i) const noexcept { return dt_tiprof_alias(p_, i); }
  bool get(const char *cap, DT_TIValue &v) const noexcept { return dt_tiprof_get(p_, cap, &v); }
  bool has(const char *cap) const noexcept { return dt_tiprof_has(p_, cap); }
  bool cap_info(const char *cap, DT_TICapInfo &i) const noexcept { return dt_tiprof_cap_info(p_, cap, &i); }
  std::size_t expand(const char *cap, const std::int32_t *params, std::size_t n, char *out, std::size_t capsz, Error *e = nullptr) const noexcept { return dt_tiprof_expand(p_, cap, params, n, out, capsz, e); }
  std::size_t expand(const std::string &cap, const std::vector<std::int32_t> &params = {}, std::string *out = nullptr, Error *e = nullptr) const {
    std::size_t n = dt_tiprof_expand(p_, cap.c_str(), params.data(), params.size(), nullptr, 0, e);
    if (n == SIZE_MAX) return n;
    if (out) { std::vector<char> buf(n + 1, '\0'); dt_tiprof_expand(p_, cap.c_str(), params.data(), params.size(), buf.data(), buf.size(), e); out->assign(buf.data(), n); }
    return n;
  }
  static std::size_t capability_count() noexcept { return dt_tiprof_cap_count(); }
  static bool capability(std::size_t i, DT_TICapInfo &x) noexcept { return dt_tiprof_cap_by_index(i, &x); }
};

inline std::unique_ptr<Profile> TIDB::load(const char *n, Error *e) { auto p=dt_tiprof_load(p_,n,e); return p?std::unique_ptr<Profile>(new Profile(p)):nullptr; }
inline std::unique_ptr<Profile> TIDB::load_default(Error *e) { auto p=dt_tidb_load_default(p_,e); return p?std::unique_ptr<Profile>(new Profile(p)):nullptr; }

class TIParser : public Handle<DT_TIParser, dt_tiparser_free> {
  explicit TIParser(DT_TIParser *p) noexcept : Handle(p) {}
public:
  static std::unique_ptr<TIParser> create(Error *e = nullptr) { auto p=dt_tiparser_create(e); return p?std::unique_ptr<TIParser>(new TIParser(p)):nullptr; }
  void reset() noexcept { dt_tiparser_reset(p_); }
  Status feed(const std::uint8_t *d, std::size_t n, bool final, std::size_t *used = nullptr, Error *e = nullptr) noexcept { return dt_tiparser_feed(p_,d,n,final,used,e); }
  std::unique_ptr<Profile> take_profile() noexcept { auto p=dt_tiparser_take_profile(p_); return p?std::unique_ptr<Profile>(new Profile(p)):nullptr; }
};

class TTY : public Handle<DT_TTYSession, dt_tty_close> {
  explicit TTY(DT_TTYSession *p) noexcept : Handle(p) {}
public:
  static void options_init(DT_TTYOptions &o) noexcept { dt_tty_options_init(&o); }
  static std::unique_ptr<TTY> open(const DT_TTYOptions &o, Error *e = nullptr) { auto p=dt_tty_open(&o,e); return p?std::unique_ptr<TTY>(new TTY(p)):nullptr; }
  int fd() const noexcept { return dt_tty_fd(p_); }
  int pollfd() const noexcept { return dt_tty_pollfd(p_); }
  Status get_mode(DT_TTYMode &m, Error *e=nullptr) noexcept { return dt_tty_get_mode(p_,&m,e); }
  Status set_mode(const DT_TTYMode &m, Error *e=nullptr) noexcept { return dt_tty_set_mode(p_,&m,e); }
  Status set_raw(Error *e=nullptr) noexcept { return dt_tty_set_raw(p_,e); }
  Status set_cooked(Error *e=nullptr) noexcept { return dt_tty_set_cooked(p_,e); }
  Status save(Error *e=nullptr) noexcept { return dt_tty_save(p_,e); }
  Status restore(Error *e=nullptr) noexcept { return dt_tty_restore(p_,e); }
  Status get_winsize(DT_Winsize &s, Error *e=nullptr) noexcept { return dt_tty_get_winsize(p_,&s,e); }
  Status set_winsize(const DT_Winsize &s, Error *e=nullptr) noexcept { return dt_tty_set_winsize(p_,&s,e); }
  Status set_nonblocking(bool b, Error *e=nullptr) noexcept { return dt_tty_set_nonblocking(p_,b,e); }
  Status read(void *b,std::size_t n,std::size_t &got,Error *e=nullptr) noexcept { return dt_tty_read(p_,b,n,&got,e); }
  Status write(const void *b,std::size_t n,std::size_t &put,Error *e=nullptr) noexcept { return dt_tty_write(p_,b,n,&put,e); }
};

class PTY : public Handle<DT_PTYSession, dt_pty_close> {
  explicit PTY(DT_PTYSession *p) noexcept : Handle(p) {}
public:
  static void options_init(DT_PTYOptions &o) noexcept { dt_pty_options_init(&o); }
  static std::unique_ptr<PTY> spawn(const DT_PTYOptions &o, Error *e=nullptr) { auto p=dt_pty_spawn(&o,e); return p?std::unique_ptr<PTY>(new PTY(p)):nullptr; }
  Status info(DT_PTYInfo &i, Error *e=nullptr) const noexcept { return dt_pty_get_info(p_,&i,e); }
  int fd() const noexcept { return dt_pty_pollfd(p_); }
  Status read(void *b,std::size_t n,std::size_t &got,Error *e=nullptr) noexcept { return dt_pty_read(p_,b,n,&got,e); }
  Status write(const void *b,std::size_t n,std::size_t &put,Error *e=nullptr) noexcept { return dt_pty_write(p_,b,n,&put,e); }
  Status resize(unsigned short r,unsigned short c,Error *e=nullptr) noexcept { return dt_pty_resize(p_,r,c,e); }
  Status get_winsize(DT_Winsize &s,Error *e=nullptr) const noexcept { return dt_pty_get_winsize(p_,&s,e); }
  Status set_nonblocking(bool b,Error *e=nullptr) noexcept { return dt_pty_set_nonblocking(p_,b,e); }
  Status wait(int &code,bool &normal,Error *e=nullptr) noexcept { return dt_pty_wait(p_,&code,&normal,e); }
  Status poll(bool &exited,int &code,bool &normal,Error *e=nullptr) noexcept { return dt_pty_poll(p_,&exited,&code,&normal,e); }
  Status terminate(int sig,Error *e=nullptr) noexcept { return dt_pty_terminate(p_,sig,e); }
};

class Recorder : public Handle<DT_Recorder, dt_recorder_free> {
  explicit Recorder(DT_Recorder *p) noexcept : Handle(p) {}
public:
  static std::unique_ptr<Recorder> create(FILE *f,bool own,Error *e=nullptr) { auto p=dt_recorder_create(f,own,e); return p?std::unique_ptr<Recorder>(new Recorder(p)):nullptr; }
  Status write(const DT_RecordEvent &x,Error *e=nullptr) noexcept { return dt_recorder_write(p_,&x,e); }
  Status flush(Error *e=nullptr) noexcept { return dt_recorder_flush(p_,e); }
};

class Replayer : public Handle<DT_Replayer, dt_replayer_free> {
  explicit Replayer(DT_Replayer *p) noexcept : Handle(p) {}
public:
  static std::unique_ptr<Replayer> create(FILE *f,bool own,Error *e=nullptr) { auto p=dt_replayer_create(f,own,e); return p?std::unique_ptr<Replayer>(new Replayer(p)):nullptr; }
  Status next(DT_RecordEvent &x,Error *e=nullptr) noexcept { return dt_replayer_next(p_,&x,e); }
  Status step(DT_RecordEvent &x,Error *e=nullptr) noexcept { return dt_replayer_step(p_,&x,e); }
  void release(DT_RecordEvent *x=nullptr) noexcept { dt_replayer_release_event(p_,x); }
  DT_ReplayMode mode() const noexcept { return dt_replayer_get_mode(p_); }
  void mode(DT_ReplayMode m) noexcept { dt_replayer_set_mode(p_,m); }
  void speed(double s) noexcept { dt_replayer_set_speed(p_,s); }
  void pause() noexcept { dt_replayer_pause(p_); } void resume() noexcept { dt_replayer_resume(p_); }
  Status rewind(Error *e=nullptr) noexcept { return dt_replayer_rewind(p_,e); }
  const char *compile(const char *t,DT_ReplayerTarget target) noexcept { return dt_replayer_compile(p_,t,target); }
};

class Message : public Handle<DT_Message, dt_message_free> {
  explicit Message(DT_Message *p) noexcept : Handle(p) {}
  friend class Envelope;
public:
  static std::unique_ptr<Message> create(DT_MessageType t,const void *d,std::size_t n,Error *e=nullptr) { auto p=dt_message_create(t,d,n,e); return p?std::unique_ptr<Message>(new Message(p)):nullptr; }
  static std::unique_ptr<Message> resize(unsigned short r,unsigned short c,Error *e=nullptr) { auto p=dt_message_create_resize(r,c,e); return p?std::unique_ptr<Message>(new Message(p)):nullptr; }
  static std::unique_ptr<Message> heartbeat(Error *e=nullptr) { auto p=dt_message_create_heartbeat(e); return p?std::unique_ptr<Message>(new Message(p)):nullptr; }
  static std::unique_ptr<Message> error(int code,const char *text,Error *e=nullptr) { auto p=dt_message_create_error_msg(code,text,e); return p?std::unique_ptr<Message>(new Message(p)):nullptr; }
  DT_MessageType type() const noexcept { return dt_message_type(p_); }
  const std::uint8_t *data(std::size_t *n=nullptr) const noexcept { return dt_message_data(p_,n); }
  Status resize(unsigned short &r,unsigned short &c) const noexcept { return dt_message_resize(p_,&r,&c); }
};

class Envelope : public Handle<DT_Envelope, dt_envelope_free> {
  explicit Envelope(DT_Envelope *p) noexcept : Handle(p) {}
  friend class LocalConnection; friend class RemoteConnection;
public:
  static std::unique_ptr<Envelope> create(std::uint64_t id,std::unique_ptr<Message> m,Error *e=nullptr) { if (!m) return nullptr; DT_Message *raw = m->get(); auto p=dt_envelope_create(id,raw,e); if (p) m.release(); return p?std::unique_ptr<Envelope>(new Envelope(p)):nullptr; }
  std::uint64_t id() const noexcept { return dt_envelope_message_id(p_); }
  DT_Message *message_handle() const noexcept { return dt_envelope_message(p_); }
  std::unique_ptr<Message> take_message() noexcept { auto p=dt_envelope_take_message(p_); return p?std::unique_ptr<Message>(new Message(p)):nullptr; }
};

class LocalConnection : public Handle<DT_LocalConn, dt_localconn_close> {
  explicit LocalConnection(DT_LocalConn *p) noexcept : Handle(p) {}
public:
  static void options_init(DT_LocalConnOptions &o) noexcept { dt_localconn_options_init(&o); }
  static std::unique_ptr<LocalConnection> open(const DT_LocalConnOptions &o,Error *e=nullptr) { auto p=dt_localconn_open(&o,e); return p?std::unique_ptr<LocalConnection>(new LocalConnection(p)):nullptr; }
  Status send(const Envelope &x,Error *e=nullptr) noexcept { return dt_localconn_send(p_,x.get(),e); }
  Status receive(std::unique_ptr<Envelope> &x,Error *e=nullptr) noexcept { DT_Envelope *p=nullptr; auto s=dt_localconn_receive(p_,&p,e); x.reset(p?new Envelope(p):nullptr); return s; }
  int fd() const noexcept { return dt_localconn_fd(p_); } DT_ConnState state() const noexcept { return dt_localconn_state(p_); } void cancel() noexcept { dt_localconn_cancel(p_); }
  Status ping(std::uint64_t id,Error *e=nullptr) noexcept { return dt_localconn_ping(p_,id,e); }
};

class RemoteConnection : public Handle<DT_RemoteConn, dt_remoteconn_close> {
  explicit RemoteConnection(DT_RemoteConn *p) noexcept : Handle(p) {}
public:
  static void options_init(DT_RemoteConnOptions &o) noexcept { dt_remoteconn_options_init(&o); }
  static std::unique_ptr<RemoteConnection> open(const DT_RemoteConnOptions &o,Error *e=nullptr) { auto p=dt_remoteconn_open(&o,e); return p?std::unique_ptr<RemoteConnection>(new RemoteConnection(p)):nullptr; }
  Status send(const Envelope &x,Error *e=nullptr) noexcept { return dt_remoteconn_send(p_,x.get(),e); }
  Status receive(std::unique_ptr<Envelope> &x,Error *e=nullptr) noexcept { DT_Envelope *p=nullptr; auto s=dt_remoteconn_receive(p_,&p,e); x.reset(p?new Envelope(p):nullptr); return s; }
  int fd() const noexcept { return dt_remoteconn_fd(p_); } DT_ConnState state() const noexcept { return dt_remoteconn_state(p_); } void cancel() noexcept { dt_remoteconn_cancel(p_); }
  Status ping(std::uint64_t id,Error *e=nullptr) noexcept { return dt_remoteconn_ping(p_,id,e); }
};

class Puppeteer : public Handle<DT_Puppeteer, dt_puppeteer_free> {
  explicit Puppeteer(DT_Puppeteer *p) noexcept : Handle(p) {}
public:
  static void options_init(DT_PuppeteerOptions &o) noexcept { dt_puppeteer_options_init(&o); }
  static std::unique_ptr<Puppeteer> create(const DT_PuppeteerOptions &o,Error *e=nullptr) { auto p=dt_puppeteer_create(&o,e); return p?std::unique_ptr<Puppeteer>(new Puppeteer(p)):nullptr; }
  DT_PTYSession *pty_handle() const noexcept { return dt_puppeteer_pty(p_); }
  DT_TIProf *profile_handle() const noexcept { return dt_puppeteer_profile(p_); }
  DT_Recorder *recorder_handle() const noexcept { return dt_puppeteer_recorder(p_); }
  Status pump_once(int ms,Error *e=nullptr) noexcept { return dt_puppeteer_pump_once(p_,ms,e); }
  Status resize(unsigned short r,unsigned short c,Error *e=nullptr) noexcept { return dt_puppeteer_resize(p_,r,c,e); }
  void request_resize(unsigned short r,unsigned short c) noexcept { dt_puppeteer_request_resize(p_,r,c); }
  Status wait(int &code,bool &normal,Error *e=nullptr) noexcept { return dt_puppeteer_wait(p_,&code,&normal,e); }
};

class TermVM : public Handle<DT_TermVM, dt_termscript_free> {
  explicit TermVM(DT_TermVM *p) noexcept : Handle(p) {}
public:
  static std::unique_ptr<TermVM> create(Error *e=nullptr) { auto p=dt_termscript_create(e); return p?std::unique_ptr<TermVM>(new TermVM(p)):nullptr; }
  Status run_string(const char *s,char **out=nullptr,Error *e=nullptr) noexcept { return dt_termscript_run_string(p_,s,out,e); }
  Status run_file(const char *s,char **out=nullptr,Error *e=nullptr) noexcept { return dt_termscript_run_file(p_,s,out,e); }
  const char *compile_to_c(const char *s,const char *unit,Error *e=nullptr) noexcept { return dt_termscript_compile_to_c(p_,s,unit,e); }
  const char *output() const noexcept { return dt_rt_output(p_); }
};

using Database = TIDB;
using TIProfile = Profile;
using Parser = TIParser;
using TTYSession = TTY;
using PTYSession = PTY;
using LocalConn = LocalConnection;
using RemoteConn = RemoteConnection;

} // namespace termlib

#endif
