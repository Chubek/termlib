"""Generate real Q headers, compile strict C11, and exercise module contracts."""
import os
from pathlib import Path
import struct
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from q import QCompiler, QError

MODULES = 'process binfile elf pe macho dlopen visitor parsec bignum schema messaging daemon evloop manpage server perf egraph'.split()
PARAMS = {'visitor': {'T': 'int', 'EQ': 'eq'}, 'messaging': {'T': 'int'}, 'egraph': {'EQ': 'eq'}}


def header(tmp_path, name, **params):
    values = {**PARAMS.get(name, {}), **params}
    text = QCompiler([str(ROOT)]).compile(str(ROOT / f'{name}.q'), **values)
    path = tmp_path / f'{values.get("PREFIX", name)}.h'
    path.write_text(text)
    return path.name


def run_c(tmp_path, source):
    src = tmp_path / 'main.c'
    src.write_text(source)
    exe = tmp_path / 'main'
    flags = ['-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror', '-pedantic']
    if os.environ.get('QLIB_SANITIZE'):
        flags += ['-fsanitize=undefined', '-fno-sanitize-recover=all']
    result = subprocess.run(['cc', *flags, str(src), '-pthread', '-ldl', '-lm', '-o', str(exe)], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([str(exe)], cwd=tmp_path, capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stdout + result.stderr


@pytest.mark.parametrize('name', MODULES)
def test_standalone_and_two_instantiations(tmp_path, name):
    a = header(tmp_path, name)
    b = header(tmp_path, name, PREFIX='other')
    run_c(tmp_path, f'''static inline int eq(int a,int b) {{ return a==b; }}
#include "{a}"
#include "{a}"
#include "{b}"
int main(void) {{ return 0; }}
''')


@pytest.mark.parametrize('name,param', [('visitor','CAP'), ('messaging','CAP'), ('evloop','CAP'), ('egraph','CAP'), ('egraph','ARITY')])
def test_invalid_capacity(name, param):
    with pytest.raises(QError):
        QCompiler().compile(str(ROOT / f'{name}.q'), **PARAMS.get(name, {}), **{param: '0'})


def test_all_headers_together(tmp_path):
    includes = '\n'.join(f'#include "{header(tmp_path, name)}"' for name in MODULES)
    run_c(tmp_path, 'static inline int eq(int a,int b) {return a==b;}\n' + includes + '\nint main(void) {return 0;}')


def test_binfile(tmp_path):
    header(tmp_path, 'binfile')
    run_c(tmp_path, r'''
#include "binfile.h"
#include <assert.h>
int main(void) {
    unsigned char bytes[]={0x80,0x12,0x34,0x56,0x78};
    binfile_t b,s; uint64_t x=99;
    assert(binfile_init(&b,bytes,sizeof bytes));
    assert(binfile_uint(&b,1,4,1,&x) && x==0x12345678);
    assert(binfile_uint(&b,1,4,0,&x) && x==0x78563412);
    assert(!binfile_uint(&b,2,4,0,&x) && x==0x78563412);
    assert(!binfile_range(&b,SIZE_MAX,2));
    assert(!binfile_uint(&b,0,9,0,&x));
    assert(binfile_slice(&b,5,0,&s) && s.size==0);
    assert(!binfile_init(&b,NULL,1));
    assert(binfile_init(&b,NULL,0) && binfile_slice(&b,0,0,&s));
    assert(binfile_read(&b,0,NULL,0));
    return 0;
}
''')


def elf_fixture(bits, big):
    order = '>' if big else '<'
    ident = b'\x7fELF' + bytes([1 if bits == 32 else 2, 2 if big else 1, 1]) + bytes(9)
    h, ph, sh = (52, 32, 40) if bits == 32 else (64, 56, 64)
    offset = h + ph + sh
    if bits == 32:
        eh = struct.pack(order+'HHIIIIIHHHHHH', 2, 3, 1, 0x1234, h, h+ph, 0, h, ph, 1, sh, 1, 0)
        seg = struct.pack(order+'IIIIIIII', 1, offset, 0x4000, 0, 3, 4, 5, 1)
        sec = struct.pack(order+'IIIIIIIIII', 0, 1, 0, 0x4000, offset, 3, 0, 0, 1, 0)
    else:
        eh = struct.pack(order+'HHIQQQIHHHHHH', 2, 62, 1, 0x1234, h, h+ph, 0, h, ph, 1, sh, 1, 0)
        seg = struct.pack(order+'IIQQQQQQ', 1, 5, offset, 0x4000, 0, 3, 4, 1)
        sec = struct.pack(order+'IIQQQQIIQQ', 0, 1, 0, 0x4000, offset, 3, 0, 0, 1, 0)
    return ident + eh + seg + sec + b'abc'


def cbytes(data):
    return ','.join(str(b) for b in data)


@pytest.mark.parametrize('bits,big', [(32,False), (32,True), (64,False), (64,True)])
def test_elf(tmp_path, bits, big):
    header(tmp_path, 'elf')
    data = elf_fixture(bits, big)
    run_c(tmp_path, f'''
#include "elf.h"
#include <assert.h>
int main(void) {{
    unsigned char data[]={{{cbytes(data)}}}; elf_t e; elf_segment p; elf_section s;
    assert(elf_init(&e,data,sizeof data) && e.bits=={bits} && e.big=={int(big)} && e.entry==0x1234);
    assert(elf_segment_at(&e,0,&p) && p.vaddr==0x4000 && p.filesz==3);
    assert(elf_section_at(&e,0,&s) && s.size==3 && s.type==1);
    assert(!elf_segment_at(&e,1,&p) && !elf_section_at(&e,1,&s));
    for (size_t n=0;n<{len(data)-3};++n) assert(!elf_init(&e,data,n));
    assert(elf_init(&e,data,sizeof data-1));
    assert(!elf_segment_at(&e,0,&p) && !elf_section_at(&e,0,&s));
    data[4]=9; assert(!elf_init(&e,data,sizeof data));
    return 0;
}}
''')


@pytest.mark.parametrize('bits', [32, 64])
def test_pe(tmp_path, bits):
    header(tmp_path, 'pe')
    optsize = 96 if bits == 32 else 112
    data = bytearray(512 + 3)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, 64)
    data[64:68] = b'PE\0\0'
    struct.pack_into('<HHIIIHH', data, 68, 0x14c if bits == 32 else 0x8664, 1, 0, 0, 0, optsize, 2)
    struct.pack_into('<H', data, 88, 0x10b if bits == 32 else 0x20b)
    struct.pack_into('<I', data, 104, 0x1000)
    struct.pack_into('<I' if bits == 32 else '<Q', data, 116 if bits == 32 else 112, 0x400000)
    struct.pack_into('<I', data, 148, 512)
    o = 88 + optsize
    data[o:o+8] = b'.text\0\0\0'
    struct.pack_into('<IIII', data, o+8, 8, 0x1000, 3, 512)
    data[512:] = b'abc'
    run_c(tmp_path, f'''
#include "pe.h"
#include <assert.h>
int main(void) {{
    unsigned char data[]={{{cbytes(data)}}}; pe_t p; pe_section s; size_t off=77;
    assert(pe_init(&p,data,sizeof data) && p.bits=={bits} && p.image_base==0x400000);
    assert(pe_section_at(&p,0,&s) && !strcmp(s.name,".text"));
    assert(pe_rva_offset(&p,0x1001,&off) && off==513);
    assert(!pe_rva_offset(&p,0x1003,&off) && off==513);
    assert(pe_rva_offset(&p,60,&off) && off==60);
    assert(!pe_section_at(&p,1,&s));
    for (size_t n=0;n<512;++n) assert(!pe_init(&p,data,n));
    assert(pe_init(&p,data,sizeof data-1) && !pe_section_at(&p,0,&s));
    memset(data+60,255,4); assert(!pe_init(&p,data,sizeof data));
    return 0;
}}
''')


@pytest.mark.parametrize('bits,big', [(32,False), (32,True), (64,False), (64,True)])
def test_macho(tmp_path, bits, big):
    header(tmp_path, 'macho')
    order = '>' if big else '<'
    data = struct.pack(order+'IIIIIII', 0xfeedface if bits == 32 else 0xfeedfacf, 7, 3, 2, 1, 8, 0)
    if bits == 64:
        data += bytes(4)
    data += struct.pack(order+'II', 0x1b, 8)
    run_c(tmp_path, f'''
#include "macho.h"
#include <assert.h>
int main(void) {{
    unsigned char data[]={{{cbytes(data)}}}; macho_t m; macho_command c;
    assert(macho_init(&m,data,sizeof data) && m.bits=={bits} && m.big=={int(big)});
    assert(macho_command_at(&m,0,&c) && c.cmd==0x1b && c.bytes.size==8);
    assert(!macho_command_at(&m,1,&c));
    for (size_t n=0;n<sizeof data;++n) assert(!macho_init(&m,data,n));
    memset(data+sizeof data-4,0,4); assert(!macho_init(&m,data,sizeof data));
    return 0;
}}
''')


def test_bignum(tmp_path):
    header(tmp_path, 'bignum')
    run_c(tmp_path, r'''
#include "bignum.h"
#include <assert.h>
int main(void) {
    bignum_t a,b,c; char text[256]="unchanged"; uint32_t rem=99;
    bignum_init(&a); bignum_init(&b); bignum_init(&c);
    assert(bignum_from_decimal(&a,"18446744073709551616"));
    assert(bignum_copy(&b,&a) && bignum_mul(&a,&a,&b));
    assert(bignum_to_decimal(&a,text,sizeof text));
    assert(!strcmp(text,"340282366920938463463374607431768211456"));
    assert(bignum_div_u32(&a,&a,10,&rem) && rem==6);
    assert(bignum_to_decimal(&a,text,sizeof text));
    assert(!strcmp(text,"34028236692093846346337460743176821145"));
    assert(!bignum_from_decimal(&a,"12x") && !bignum_div_u32(&a,&a,0,&rem));
    assert(!bignum_sub(&c,&b,&a) && !c.size);
    assert(bignum_sub(&a,&a,&a) && !a.size);
    assert(bignum_set_u64(&a,UINT64_MAX) && bignum_set_u64(&b,1));
    assert(bignum_add(&a,&a,&b));
    assert(bignum_to_decimal(&a,text,sizeof text) && !strcmp(text,"18446744073709551616"));
    assert(bignum_sub(&a,&a,&b));
    assert(bignum_to_decimal(&a,text,sizeof text) && !strcmp(text,"18446744073709551615"));
    assert(!bignum_to_decimal(&a,text,2) && !strcmp(text,"18446744073709551615"));
    assert(bignum_from_decimal(&a,"0000") && !a.size);
    assert(bignum_to_decimal(&a,text,sizeof text) && !strcmp(text,"0"));
    bignum_free(&a); bignum_free(&b); bignum_free(&c); return 0;
}
''')


def test_parsec(tmp_path):
    header(tmp_path, 'parsec')
    run_c(tmp_path, r'''
#include "parsec.h"
#include <assert.h>
static int token(parsec_t *p,void *ctx) { return parsec_byte(p,*(unsigned char *)ctx); }
static int empty(parsec_t *p,void *ctx) { (void)p; (void)ctx; return 1; }
int main(void) {
    parsec_t p; uint64_t v=77; size_t n=99; unsigned char a='a',b='b';
    assert(parsec_init(&p,"aaab",4));
    assert(parsec_many(&p,token,&a,1,10,&n) && n==3 && p.pos==3);
    assert(parsec_choice(&p,token,&a,token,&b) && parsec_eof(&p));
    assert(parsec_init(&p,"ac",2));
    assert(!parsec_sequence(&p,token,&a,token,&b) && p.pos==0);
    assert(!parsec_many(&p,token,&a,2,10,&n) && p.pos==0 && n==3);
    assert(!parsec_many(&p,empty,NULL,0,10,&n) && p.pos==0);
    assert(parsec_init(&p,"18446744073709551616",20));
    assert(!parsec_uint(&p,&v) && p.pos==0 && v==77);
    assert(parsec_init(&p,"18446744073709551615!",21));
    assert(parsec_uint(&p,&v) && v==UINT64_MAX && p.pos==20);
    assert(!parsec_literal(&p,"long",4) && p.pos==20);
    return 0;
}
''')


def test_visitor(tmp_path):
    header(tmp_path, 'visitor', CAP='3')
    run_c(tmp_path, r'''
static inline int eq(int a,int b) {return a==b;}
#include "visitor.h"
#include <assert.h>
static size_t count(int n,void *ctx) { (void)n; (void)ctx; return 1; }
static int child(int n,size_t i,void *ctx) { (void)i; (void)ctx; return (n+1)%3; }
static int next(int n,size_t i,void *ctx) { (void)i; (void)ctx; return n+1; }
static int visit(int n,int leave,void *ctx) { int *v=ctx; v[(*v)+1]=leave ? -n-1 : n+1; ++*v; return 1; }
static int stop(int n,int leave,void *ctx) { (void)n; (void)leave; (void)ctx; return 0; }
int main(void) {
    int log[16]={0};
    assert(visitor_walk(0,count,child,visit,log)==1 && log[0]==6);
    assert(log[1]==1 && log[3]==3 && log[4]==-3 && log[6]==-1);
    log[0]=0; assert(visitor_walk(0,count,next,visit,log)==-1);
    assert(visitor_walk(0,count,child,stop,log)==0);
    return 0;
}
''')


def test_schema(tmp_path):
    header(tmp_path, 'schema')
    run_c(tmp_path, r'''
#include "schema.h"
#include <assert.h>
int main(void) {
    schema_field f[]={ {"age",schema_INT,1,1,0,130,0}, {"name",schema_STRING,0,0,0,0,4} };
    schema_value v[]={ {"age",schema_INT,{.integer=42}}, {"name",schema_STRING,{.string="Ada"}} };
    schema_error e;
    assert(schema_validate(f,2,v,2,1,&e) && e.code==schema_OK);
    v[0].as.integer=131; assert(!schema_validate(f,2,v,2,1,&e) && e.code==schema_RANGE && e.field==0);
    v[0].as.integer=42; v[1].name="age";
    assert(!schema_validate(f,2,v,2,1,&e) && e.code==schema_DUPLICATE);
    v[1].name="extra"; assert(!schema_validate(f,2,v,2,1,&e) && e.code==schema_UNKNOWN);
    assert(schema_validate(f,2,v,2,0,&e));
    assert(!schema_validate(f,2,NULL,0,1,&e) && e.code==schema_MISSING);
    v[0].kind=schema_NUMBER; assert(!schema_validate(f,2,v,1,1,&e) && e.code==schema_TYPE);
    f[0].kind=schema_NUMBER; v[0].as.number=NAN;
    assert(!schema_validate(f,2,v,1,1,&e) && e.code==schema_RANGE);
    return 0;
}
''')


def test_egraph(tmp_path):
    header(tmp_path, 'egraph', CAP='8', ARITY='2')
    run_c(tmp_path, r'''
static inline int eq(int a,int b) {return a==b;}
#include "egraph.h"
#include <assert.h>
static size_t cost(int op,void *ctx) { (void)ctx; return op==3 ? 2 : 1; }
int main(void) {
    egraph_t g; size_t a,b,fa,fb,ffa,ffb,best[8],total=0; egraph_init(&g);
    a=egraph_add(&g,1,NULL,0); b=egraph_add(&g,2,NULL,0);
    assert(a==egraph_add(&g,1,NULL,0));
    fa=egraph_add(&g,3,&a,1); fb=egraph_add(&g,3,&b,1);
    ffa=egraph_add(&g,4,&fa,1); ffb=egraph_add(&g,4,&fb,1);
    assert(!egraph_equal(&g,fa,fb) && egraph_merge(&g,a,b)==1);
    assert(egraph_equal(&g,fa,fb) && egraph_equal(&g,ffa,ffb));
    assert(egraph_extract(&g,ffa,cost,NULL,best,&total) && total==4);
    assert(egraph_merge(&g,a,fa)==1); /* cyclic equivalence still has a finite leaf */
    assert(egraph_extract(&g,fa,cost,NULL,best,&total) && total==1 && best[fa]==a);
    assert(egraph_merge(&g,a,SIZE_MAX)==-1 && !egraph_equal(&g,SIZE_MAX,SIZE_MAX));
    assert(egraph_add(&g,99,NULL,1)==SIZE_MAX);
    assert(egraph_add(&g,90,NULL,0)!=SIZE_MAX && egraph_add(&g,91,NULL,0)!=SIZE_MAX);
    assert(egraph_add(&g,92,NULL,0)==SIZE_MAX);
    return 0;
}
''')


def test_process(tmp_path):
    header(tmp_path, 'process')
    run_c(tmp_path, r'''
#include "process.h"
#include <assert.h>
int main(void) {
    process_t p; int status; char *env[]={NULL};
    char *args[]={"sh","-c","exit 7",NULL}; char *bad[]={"/no/such/q/program",NULL};
    process_init(&p); assert(process_spawn(&p,bad[0],bad,env)==-1 && !p.live);
    assert(process_spawn(&p,"sh",args,env)==0);
    assert(process_spawn(&p,"sh",args,env)==-1 && p.live);
    assert(process_wait(&p,&status,0)==1 && process_exit_code(status)==7);
    assert(process_wait(&p,&status,0)==-1 && process_signal(&p,SIGTERM)==-1);
    args[2]="exec sleep 5"; assert(process_spawn(&p,"sh",args,env)==0);
    assert(process_signal(&p,SIGTERM)==0 && process_wait(&p,&status,0)==1);
    assert(WIFSIGNALED(status) && process_exit_code(status)==-1);
    return 0;
}
''')


def test_dlopen(tmp_path):
    header(tmp_path, 'dlopen')
    run_c(tmp_path, r'''
#include "dlopen.h"
#include <assert.h>
int main(void) {
    dlopen_t d; void *symbol=(void *)1;
    dlopen_init(&d); assert(!dlopen_open(&d,"/no/such/q/library.so",RTLD_NOW));
    assert(dlopen_error()!=NULL);
    assert(dlopen_open(&d,NULL,RTLD_NOW) && !dlopen_open(&d,NULL,RTLD_NOW));
    assert(dlopen_symbol(&d,"malloc",&symbol) && symbol);
    assert(!dlopen_symbol(&d,"q_missing_symbol_1234",&symbol));
    assert(dlopen_close(&d) && !dlopen_close(&d));
    return 0;
}
''')


def test_messaging(tmp_path):
    header(tmp_path, 'messaging', CAP='2')
    run_c(tmp_path, r'''
#include "messaging.h"
#include <assert.h>
static void *producer(void *arg) {
    messaging_t *q=arg;
    for (int i=0;i<1000;++i) assert(messaging_send(q,i,1)==1);
    messaging_close(q); return NULL;
}
static void *blocked_sender(void *arg) { assert(messaging_send(arg,3,1)==0); return NULL; }
static void *blocked_receiver(void *arg) { int n; assert(messaging_recv(arg,&n,1)==0); return NULL; }
int main(void) {
    messaging_t q; pthread_t thread; int n,rc,total=0;
    assert(!messaging_init(&q));
    assert(messaging_recv(&q,&n,0)==-1);
    assert(messaging_send(&q,1,0)==1 && messaging_send(&q,2,0)==1 && messaging_send(&q,3,0)==-1);
    assert(!pthread_create(&thread,NULL,blocked_sender,&q)); messaging_close(&q); pthread_join(thread,NULL);
    assert(messaging_recv(&q,&n,0)==1 && n==1);
    assert(messaging_recv(&q,&n,0)==1 && n==2);
    assert(messaging_recv(&q,&n,0)==0); messaging_destroy(&q);
    assert(!messaging_init(&q)); assert(!pthread_create(&thread,NULL,producer,&q));
    while ((rc=messaging_recv(&q,&n,1))==1) total+=n;
    assert(rc==0 && total==499500); pthread_join(thread,NULL); messaging_destroy(&q);
    assert(!messaging_init(&q)); assert(!pthread_create(&thread,NULL,blocked_receiver,&q));
    messaging_close(&q); pthread_join(thread,NULL); messaging_destroy(&q);
    return 0;
}
''')


def test_evloop(tmp_path):
    header(tmp_path, 'evloop', CAP='2')
    run_c(tmp_path, r'''
#include "evloop.h"
#include <assert.h>
#include <unistd.h>
typedef struct { evloop_t *loop; int other, count; } context;
static void later(int fd,short events,void *arg) {
    context *c=arg; char byte;
    assert(events&POLLIN); assert(read(fd,&byte,1)==1); ++c->count;
    evloop_stop(c->loop);
}
static void first(int fd,short events,void *arg) {
    context *c=arg; char byte;
    assert(events&POLLIN); assert(read(fd,&byte,1)==1); ++c->count;
    assert(evloop_once(c->loop,0)==-1 && errno==EBUSY);
    assert(evloop_remove(c->loop,c->other));
    assert(evloop_add(c->loop,c->other,POLLIN,later,c));
}
int main(void) {
    evloop_t loop; int a[2],b[2]; context c={&loop,0,0};
    assert(!pipe(a) && !pipe(b)); evloop_init(&loop); c.other=b[0];
    assert(evloop_add(&loop,a[0],POLLIN,first,&c));
    assert(evloop_add(&loop,b[0],POLLIN,later,&c));
    assert(!evloop_add(&loop,a[1],POLLIN,later,&c));
    assert(write(a[1],"a",1)==1 && write(b[1],"b",1)==1);
    assert(evloop_once(&loop,1000)==1 && c.count==1); /* stale callback skipped */
    assert(evloop_run(&loop,1000)==0 && c.count==2);
    close(a[0]); close(a[1]); close(b[0]); close(b[1]); return 0;
}
''')


def test_daemon_pidfile(tmp_path):
    header(tmp_path, 'daemon')
    run_c(tmp_path, r'''
#include "daemon.h"
#include <assert.h>
#include <sys/wait.h>
int main(void) {
    int fd,status; pid_t pid; char buf[64]={0};
    fd=daemon_pidfile_lock("service.pid"); assert(fd>=0);
    assert(lseek(fd,0,SEEK_SET)==0 && read(fd,buf,sizeof buf-1)>0);
    assert(buf[0]>='0' && buf[0]<='9');
    pid=fork(); assert(pid>=0);
    if (!pid) { int other=daemon_pidfile_lock("service.pid"); _exit(other<0 ? 0 : 1); }
    assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
    assert(daemon_pidfile_unlock(fd)==0);
    fd=daemon_pidfile_lock("service.pid"); assert(fd>=0); assert(!daemon_pidfile_unlock(fd));
    return 0;
}
''')


def test_daemon_detach(tmp_path):
    header(tmp_path, 'daemon')
    run_c(tmp_path, r'''
#include "daemon.h"
#include <assert.h>
#include <sys/wait.h>
int main(void) {
    int fds[2],rc,status; char result=0; assert(!pipe(fds));
    rc=daemon_detach(1,1); assert(rc>=0);
    if (!rc) {
        char cwd[4]; int ok=getcwd(cwd,sizeof cwd)!=NULL && cwd[0]=='/' && cwd[1]==0;
        char answer=ok ? 'y' : 'n'; close(fds[0]);
        if (write(fds[1],&answer,1)!=1) _exit(2);
        close(fds[1]); _exit(0);
    }
    close(fds[1]); assert(read(fds[0],&result,1)==1 && result=='y'); close(fds[0]);
    assert(wait(&status)>0 && WIFEXITED(status) && WEXITSTATUS(status)==0);
    return 0;
}
''')


def test_server(tmp_path):
    header(tmp_path, 'server')
    run_c(tmp_path, r'''
#include "server.h"
#include <assert.h>
#include <netinet/in.h>
int main(void) {
    server_t s; struct sockaddr_in addr; socklen_t len=sizeof addr; int client,peer; char byte;
    server_init(&s); assert(server_listen(&s,"127.0.0.1",65536,1)==-1);
    assert(server_listen(&s,"127.0.0.1",0,2)==0);
    assert(!getsockname(s.fd,(struct sockaddr *)&addr,&len));
    assert(server_nonblocking(s.fd,1)==0 && server_accept(&s)==-1 && (errno==EAGAIN || errno==EWOULDBLOCK));
    assert(server_nonblocking(s.fd,0)==0);
    client=socket(AF_INET,SOCK_STREAM,0); assert(client>=0);
    assert(!connect(client,(struct sockaddr *)&addr,len)); peer=server_accept(&s); assert(peer>=0);
    assert(fcntl(peer,F_GETFD)&FD_CLOEXEC);
    assert(write(client,"q",1)==1 && read(peer,&byte,1)==1 && byte=='q');
    close(client); close(peer); assert(!server_close(&s) && !server_close(&s)); return 0;
}
''')


def test_perf(tmp_path):
    header(tmp_path, 'perf')
    run_c(tmp_path, r'''
#include "perf.h"
#include <assert.h>
int main(void) {
    perf_timer t; perf_stats s; double elapsed=-1;
    perf_init(&t); assert(!perf_elapsed(&t,&elapsed));
    assert(perf_start(&t) && perf_elapsed(&t,&elapsed) && elapsed>=0);
    perf_stats_init(&s); assert(perf_variance(&s)==0);
    assert(perf_record(&s,1) && perf_record(&s,2) && perf_record(&s,3));
    assert(s.count==3 && s.mean==2 && s.min==1 && s.max==3 && perf_variance(&s)==1);
    assert(!perf_record(&s,-1) && !perf_record(&s,NAN) && !perf_record(&s,INFINITY) && s.count==3);
    return 0;
}
''')


def test_manpage(tmp_path):
    header(tmp_path, 'manpage')
    run_c(tmp_path, r'''
#include "manpage.h"
#include <assert.h>
#include <string.h>
int main(void) {
    FILE *f=tmpfile(); char buf[1024]; size_t n; assert(f);
    assert(manpage_header(f,"tool", "1", "2026-10-08"));
    assert(manpage_section(f,"NAME"));
    assert(manpage_paragraph(f,".request\n'control\nback\\slash"));
    assert(manpage_section(f,"a\"b"));
    rewind(f); n=fread(buf,1,sizeof buf-1,f); buf[n]=0;
    assert(strstr(buf,".TH \"tool\" \"1\" \"2026-10-08\"\n"));
    assert(strstr(buf,"\\&.request\n\\&'control\nback\\eslash"));
    assert(strstr(buf,".SH \"a\\(dqb\""));
    assert(!manpage_section(f,"bad\n.SH injected"));
    fclose(f); return 0;
}
''')


def test_bignum_against_python_integers(tmp_path):
    """Check limb carries/borrows and aliasing against an independent oracle."""
    import random

    header(tmp_path, 'bignum')
    rng = random.Random(1701)
    pairs = [(0, 0), (2**128-1, 2**128-1), (2**256, 1)]
    pairs += [(rng.getrandbits(rng.randrange(1, 1025)), rng.getrandbits(rng.randrange(1, 1025))) for _ in range(24)]
    checks = []
    for a, b in pairs:
        divisor = rng.randrange(1, 2**32)
        checks.append(f'''
assert(bignum_from_decimal(&a,"{a}") && bignum_from_decimal(&b,"{b}"));
assert(bignum_add(&c,&a,&b) && bignum_to_decimal(&c,text,sizeof text) && !strcmp(text,"{a+b}"));
assert(bignum_mul(&c,&a,&b) && bignum_to_decimal(&c,text,sizeof text) && !strcmp(text,"{a*b}"));
assert(bignum_div_u32(&c,&a,{divisor}u,&rem) && rem=={a%divisor}u);
assert(bignum_to_decimal(&c,text,sizeof text) && !strcmp(text,"{a//divisor}"));
assert(bignum_mul(&a,&a,&b) && bignum_to_decimal(&a,text,sizeof text) && !strcmp(text,"{a*b}"));
assert(bignum_from_decimal(&a,"{max(a,b)}") && bignum_from_decimal(&b,"{min(a,b)}"));
assert(bignum_sub(&b,&a,&b) && bignum_to_decimal(&b,text,sizeof text) && !strcmp(text,"{abs(a-b)}"));
''')
    run_c(tmp_path, '''
#include "bignum.h"
#include <assert.h>
int main(void) {
    bignum_t a,b,c; char text[1024]; uint32_t rem;
    bignum_init(&a); bignum_init(&b); bignum_init(&c);
''' + '\n'.join(checks) + '''
    bignum_free(&a); bignum_free(&b); bignum_free(&c); return 0;
}
''')
