/* libkernel/libScePosix process services: clocks, sleeping, errno mapping,
 * pthread once/keys, signal bookkeeping and a narrow sysctl. Guest values
 * use FreeBSD numbering; host errno values never reach the guest directly. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <utime.h>
#ifndef _WIN32
#include <sys/random.h>
#include <sys/resource.h>
#endif
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define PAGE 16384

typedef struct { int64_t sec, nsec; } GuestTimespec;
typedef struct { int64_t sec, usec; } GuestTimeval;

int32_t runtime_guest_errno(int e) {
    switch (e) {
    case 0: return 0;
    case EPERM: return 1; case ENOENT: return 2; case ESRCH: return 3; case EINTR: return 4;
    case EIO: return 5; case ENXIO: return 6; case E2BIG: return 7; case ENOEXEC: return 8;
    case EBADF: return 9; case ECHILD: return 10; case EDEADLK: return 11; case ENOMEM: return 12;
    case EACCES: return 13; case EFAULT: return 14; case EBUSY: return 16; case EEXIST: return 17;
    case EXDEV: return 18; case ENODEV: return 19; case ENOTDIR: return 20; case EISDIR: return 21;
    case EINVAL: return 22; case ENFILE: return 23; case EMFILE: return 24; case ENOTTY: return 25;
    case EFBIG: return 27; case ENOSPC: return 28; case ESPIPE: return 29; case EROFS: return 30;
    case EMLINK: return 31; case EPIPE: return 32; case ERANGE: return 34; case EAGAIN: return 35;
    case ENAMETOOLONG: return 63; case ENOTEMPTY: return 66; case ETIMEDOUT: return 60;
    case ELOOP: return 62; case ENOSYS: return 78; case EOVERFLOW: return 84; case ECANCELED: return 85;
    default: return 5; /* EIO: unmapped host error */
    }
}
static int32_t fail_posix(int e) { *runtime_errno()=runtime_guest_errno(e); return -1; }

/* ---- clocks and sleeping ---- */
static struct timespec process_start;
__attribute__((constructor)) static void remember_start(void) { clock_gettime(CLOCK_MONOTONIC,&process_start); }
static int host_clock(uint32_t id,clockid_t *out) {
    switch (id) {
    case 0: case 9: case 10: *out=CLOCK_REALTIME; return 1;            /* REALTIME(_PRECISE/_FAST) */
    case 4: case 11: case 12: case 5: case 7: case 8: *out=CLOCK_MONOTONIC; return 1; /* MONOTONIC/UPTIME */
    case 13: *out=CLOCK_REALTIME_COARSE; return 1;                    /* SECOND */
    case 14: *out=CLOCK_THREAD_CPUTIME_ID; return 1;
    case 2: case 15: *out=CLOCK_PROCESS_CPUTIME_ID; return 1;         /* PROF/PROCTIME */
    case 16: case 17: case 18: case 19: *out=CLOCK_MONOTONIC; return 1; /* PS4 network clocks */
    default: return 0;
    }
}
static int clock_read(uint32_t id,GuestTimespec *ts) {
    clockid_t host;
    if (!ts || !host_clock(id,&host)) return EINVAL;
    struct timespec t;
    if (clock_gettime(host,&t)) return errno;
    if (id==13) t.tv_nsec=0;
    ts->sec=t.tv_sec; ts->nsec=t.tv_nsec; return 0;
}
static ABI int32_t kernel_clock_gettime(uint32_t id,GuestTimespec *ts) {
    int e=clock_read(id,ts); return e ? ERR(runtime_guest_errno(e)) : 0;
}
static ABI int32_t posix_clock_gettime(uint32_t id,GuestTimespec *ts) {
    int e=clock_read(id,ts); return e ? fail_posix(e) : 0;
}
static ABI int32_t posix_clock_getres(uint32_t id,GuestTimespec *ts) {
    clockid_t host; struct timespec t;
    if (!host_clock(id,&host)) return fail_posix(EINVAL);
    if (clock_getres(host,&t)) return fail_posix(errno);
    if (ts) { ts->sec=t.tv_sec; ts->nsec=t.tv_nsec; }
    return 0;
}
static ABI uint64_t process_time(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)(t.tv_sec-process_start.tv_sec)*1000000+(uint64_t)((t.tv_nsec-process_start.tv_nsec)/1000);
}
static ABI uint64_t process_time_counter(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)(t.tv_sec-process_start.tv_sec)*1000000000+(uint64_t)(t.tv_nsec-process_start.tv_nsec);
}
static ABI uint64_t process_time_frequency(void) { return 1000000000; }
/* The guest's TSC is the process clock in nanoseconds. The raw rdtsc differs per core on some
 * hosts (3.9 s apart on a Xeon E5-2680 v4), so a thread moving cores made the game jump. */
static ABI uint64_t read_tsc(void) { return process_time_counter(); }
static ABI uint64_t tsc_frequency(void) { return 1000000000; }
/* Shared with the GPU library so flip/label timestamps use the guest's clock. */
uint64_t runtime_process_time_us(void) { return process_time(); }
uint64_t runtime_process_time_counter(void) { return process_time_counter(); }
uint64_t runtime_tsc_frequency(void) { return tsc_frequency(); }
uint64_t runtime_read_tsc(void) { return read_tsc(); }
static int sleep_ns(uint64_t ns) {
    if (!ns) runtime_thread_count_yield(1);
#ifdef _WIN32
    compat_sleep_ns(ns);
#else
    struct timespec t={.tv_sec=(time_t)(ns/1000000000),.tv_nsec=(long)(ns%1000000000)};
    while (nanosleep(&t,&t)) if (errno!=EINTR) return errno;
#endif
    return 0;
}
static ABI int32_t kernel_usleep(uint32_t usec) { sleep_ns((uint64_t)usec*1000); return 0; }
static ABI int32_t posix_usleep(uint32_t usec) { sleep_ns((uint64_t)usec*1000); return 0; }
static ABI uint32_t posix_sleep(uint32_t seconds) { sleep_ns((uint64_t)seconds*1000000000); return 0; }
static ABI int32_t posix_nanosleep(const GuestTimespec *rq,GuestTimespec *rem) {
    if (!rq || rq->nsec<0 || rq->nsec>=1000000000 || rq->sec<0) return fail_posix(EINVAL);
    sleep_ns((uint64_t)rq->sec*1000000000+(uint64_t)rq->nsec);
    if (rem) { rem->sec=0; rem->nsec=0; }
    return 0;
}
static ABI int32_t kernel_nanosleep(const GuestTimespec *rq,GuestTimespec *rem) {
    return posix_nanosleep(rq,rem) ? ERR(*runtime_errno()) : 0;
}
typedef struct { int32_t minuteswest, dsttime; } GuestTimezone;
static ABI int32_t kernel_gettimezone(GuestTimezone *tz) {
    if (!tz) return ERR(22);
    time_t now=time(NULL);
#ifdef _WIN32
    tz->minuteswest=(int32_t)(-compat_utc_offset(now)/60); tz->dsttime=0;
#else
    struct tm local; localtime_r(&now,&local);
    tz->minuteswest=(int32_t)(-local.tm_gmtoff/60); tz->dsttime=0;
#endif
    return 0;
}
static ABI int32_t posix_gettimeofday(GuestTimeval *tv,GuestTimezone *tz) {
    struct timeval t;
    gettimeofday(&t,NULL);
    if (tv) { tv->sec=t.tv_sec; tv->usec=t.tv_usec; }
    if (tz) kernel_gettimezone(tz);
    return 0;
}
static ABI int32_t kernel_gettimeofday(GuestTimeval *tv) {
    if (!tv) return ERR(22);
    return posix_gettimeofday(tv,NULL);
}
static ABI int64_t posix_time(int64_t *out) { int64_t t=(int64_t)time(NULL); if (out) *out=t; return t; }
/* sceKernelSleep(seconds): sceKernelUsleep of the whole seconds (shadPS4 time.cpp). */
static ABI int32_t kernel_sleep(uint32_t seconds) { sleep_ns((uint64_t)seconds*1000000000); return 0; }
/* The zone at UTC time t as libkernel splits it: the standard offset east of UTC, the summer time
 * in effect (0 outside it) and the zone's summer time amount (0 for a zone without one). */
typedef struct { int64_t standard, dst, zone_dst; } GuestZone;
static GuestZone zone_at(time_t t) {
    GuestZone z; int isdst; long offset, zone_dst;
#ifdef _WIN32
    offset=compat_zone(t,&isdst,&zone_dst);
#else
    static pthread_once_t once=PTHREAD_ONCE_INIT;
    pthread_once(&once,tzset);
    struct tm local; localtime_r(&t,&local);
    offset=local.tm_gmtoff; isdst=local.tm_isdst>0;
    zone_dst=isdst ? offset+timezone : (daylight ? 3600 : 0);
#endif
    z.zone_dst=zone_dst; z.dst=isdst ? zone_dst : 0; z.standard=offset-z.dst;
    return z;
}
/* sceKernelConvertUtcToLocaltime(utc, local, timesec{t, west_sec, dst_sec}, dst_sec): local =
 * utc + west_sec + dst_sec, where west_sec is the standard offset east of UTC and dst_sec the
 * summer time in effect. libc's localtime sets tm_isdst from dst_sec != 0. */
typedef struct { int64_t t; int32_t west_sec, dst_sec; } GuestTimesec;
static ABI int32_t kernel_utc_to_local(int64_t utc,int64_t *local,GuestTimesec *st,uint64_t *dst) {
    if (!local) return ERR(22);
    GuestZone z=zone_at((time_t)utc);
    *local=utc+z.standard+z.dst;
    if (st) { st->t=utc; st->west_sec=(int32_t)z.standard; st->dst_sec=(int32_t)z.dst; }
    if (dst) *dst=(uint64_t)z.dst;
    return 0;
}
/* sceKernelConvertLocaltimeToUtc(local, isdst, utc, timesec, dst_seconds): the UTC time whose
 * local time is `local`, with timesec filled for it as sceKernelConvertUtcToLocaltime does and
 * dst_seconds the zone's summer time amount; timesec is required (EINVAL). libc's mktime applies
 * tm_isdst itself: it converts back and adds or removes dst_seconds when the two disagree, so a
 * local time inside the spring gap needs no exact answer here. */
static ABI int32_t kernel_local_to_utc(int64_t local,int64_t isdst,int64_t *utc,GuestTimesec *st,int32_t *dst) {
    (void)isdst;
    if (!st) return ERR(22);
    GuestZone z=zone_at((time_t)local);
    int64_t guess=local-z.standard-z.dst;
    for (int i=0;i<2;i++) {
        GuestZone at=zone_at((time_t)guess);
        int64_t next=local-at.standard-at.dst;
        if (next==guess) break;
        guess=next;
    }
    z=zone_at((time_t)guess);
    if (utc) *utc=guess;
    st->t=guess; st->west_sec=(int32_t)z.standard; st->dst_sec=(int32_t)z.dst;
    if (dst) *dst=(int32_t)z.zone_dst;
    return 0;
}

/* ---- file attributes (runtime_file.c owns paths and descriptors) ---- */
/* The game's own package is read-only, as in runtime_file.c. */
static int game_path(const char *p) {
    if (!p || !*p) return 0;
    if (*p!='/') return 1;
    return (!strncmp(p,"/app0",5) && (!p[5] || p[5]=='/')) ||
           (!strncmp(p,"/hostapp",8) && (!p[8] || p[8]=='/')) ||
           (!strncmp(p,"/archive/mount/point",20) && (!p[20] || p[20]=='/'));
}
/* 0 or a host errno for an existing, writable guest path; its host path in `host`. */
static int writable_path(const char *guest,char *host,size_t size) {
    unsigned char st[256];
    if (!guest) return EFAULT;
    int64_t r=runtime_file_stat(guest,st);
    if (r) return (int)-r;
    if (game_path(guest)) return EROFS;
    return runtime_file_translate(guest,host,size);
}
static int fd_open(int fd) { unsigned char st[256]; return runtime_file_fstat(fd,st)==0; }
/* chmod(path, mode) / sceKernelChmod: the file must exist and be writable. Permission bits are
 * not kept on the host: a cleared write bit there would stop the runtime itself writing. */
static int file_chmod(const char *path) { char host[1024]; return writable_path(path,host,sizeof(host)); }
static ABI int32_t posix_chmod(const char *path,uint32_t mode) { (void)mode; int e=file_chmod(path); return e ? fail_posix(e) : 0; }
static ABI int32_t kernel_chmod(const char *path,uint32_t mode) { (void)mode; int e=file_chmod(path); return e ? ERR(runtime_guest_errno(e)) : 0; }
static ABI int32_t posix_fchmod(int fd,uint32_t mode) { (void)mode; return fd_open(fd) ? 0 : fail_posix(EBADF); }
/* utimes(path, times[2] {sec, usec}) / sceKernelUtimes: access and modification times, now
 * when times is NULL. */
static int file_utimes(const char *path,const GuestTimeval *times) {
    char host[1024];
    int e=writable_path(path,host,sizeof(host));
    if (e) return e;
    if (times && (times[0].usec<0 || times[0].usec>=1000000 || times[1].usec<0 || times[1].usec>=1000000)) return EINVAL;
    struct utimbuf t={times ? (time_t)times[0].sec : time(NULL),times ? (time_t)times[1].sec : time(NULL)};
    return utime(host,&t) ? errno : 0;
}
static ABI int32_t posix_utimes(const char *path,const GuestTimeval *times) { int e=file_utimes(path,times); return e ? fail_posix(e) : 0; }
static ABI int32_t kernel_utimes(const char *path,const GuestTimeval *times) { int e=file_utimes(path,times); return e ? ERR(runtime_guest_errno(e)) : 0; }
/* futimes(fd, times): checks the descriptor; runtime_file.c keeps no host path for it, so the
 * host times stay as they are. */
static ABI int32_t posix_futimes(int fd,const GuestTimeval *times) { (void)times; return fd_open(fd) ? 0 : fail_posix(EBADF); }
/* fcntl(fd, cmd, arg) on a file (FreeBSD numbering). No descriptor is close-on-exec (no exec):
 * F_GETFD 0, F_SETFD accepted. F_GETFL reports O_RDWR. F_SETFL: O_NONBLOCK does nothing for a
 * file; O_APPEND, O_ASYNC and O_DIRECT cannot be switched (EINVAL). The process is alone, so
 * F_SETLK/F_SETLKW always lock and F_GETLK finds the range free (l_type @20 = F_UNLCK). */
static ABI int32_t posix_fcntl(int fd,int cmd,uint64_t arg) {
    if (!fd_open(fd)) return fail_posix(EBADF);
    switch (cmd) {
    case 1: return 0;                                   /* F_GETFD */
    case 2: return 0;                                   /* F_SETFD */
    case 3: return 2;                                   /* F_GETFL: O_RDWR */
    case 4: return arg & (0x8|0x40|0x10000) ? fail_posix(EINVAL) : 0; /* F_SETFL */
    case 7: if (!arg) return fail_posix(EFAULT); *(int16_t *)(uintptr_t)(arg+20)=2; return 0; /* F_GETLK */
    case 8: case 9: return arg ? 0 : fail_posix(EFAULT); /* F_SETLK, F_SETLKW */
    default: fprintf(stderr,"STOP: unsupported fcntl command %d\n",cmd); exit(21);
    }
}
/* sceKernelGetdents(fd, buf, nbytes): sceKernelGetdirentries without the base offset. */
static ABI int64_t kernel_getdents(int fd,char *buf,uint64_t nbytes) {
    int64_t r=runtime_file_getdents(fd,buf,nbytes,NULL);
    return r<0 ? ERR(runtime_guest_errno((int)-r)) : r;
}
/* sceKernelSetGPO(bits): the development kit's LEDs; there are none (OK, as shadPS4). */
static ABI int32_t kernel_set_gpo(uint32_t bits) { (void)bits; return 0; }
/* sceKernelInternalMemoryGetModuleSegmentInfo(info{address, size}): libc asks for a reserved
 * internal-memory segment; none exists, so both are 0 (libc then leaves it unused). */
static ABI int32_t internal_segment_info(uint64_t *info) {
    if (!info) return ERR(14);
    info[0]=info[1]=0; return 0;
}

/* ---- process ---- */
static ABI int32_t get_pagesize(void) { return PAGE; }
static ABI int32_t get_pid(void) { return 1000; }
static ABI int32_t yield(void) { runtime_thread_count_yield(0); sched_yield(); return 0; }
static ABI __attribute__((noreturn)) void hard_exit(int status) {
    printf("Runtime: guest requested _exit(%d)\n",status);
    runtime_report();
    fflush(stdout);
    _exit(status);
}
static ABI __attribute__((noreturn)) void raise_exception(uint32_t code,uint64_t argument) {
    fprintf(stderr,"STOP: guest raised debug exception code=0x%x argument=0x%llx\n",code,(unsigned long long)argument);
    runtime_report();
    exit(23);
}
/* The process was started without arguments other than its own name. */
static const char *guest_argv[]={"eboot.bin",NULL};
static ABI int32_t get_argc(void) { return 1; }
static ABI const char **get_argv(void) { return guest_argv; }
/* Only the game and its bundled modules exist; nothing was loaded at run time. */
static ABI int32_t stop_unload_module(int32_t handle,uint64_t args,const void *argp,uint32_t flags,const void *opt,int32_t *result) {
    (void)args; (void)argp; (void)flags; (void)opt; (void)result;
    printf("Runtime: sceKernelStopUnloadModule(%d): no such module\n",handle);
    return ERR(3); /* ESRCH */
}
static ABI int32_t module_info_from_addr(uint64_t address,int32_t flags,void *info) {
    (void)address;
    if (flags>=3) return ERR(22);
    if (!info) return ERR(14);
    return ERR(3); /* ESRCH: the runtime keeps no module records */
}
static ABI int32_t kernel_mlock(const void *address,uint64_t size) { (void)address; (void)size; return 0; }
/* libc's C++ exception unwinder. The game does not throw (no __cxa_throw import): unwinding
 * finds no frame data, and resuming a context is a stop, never a silent success. */
static ABI int32_t elf_phdr_match_addr(void *info,uint64_t address) { (void)info; (void)address; return 0; }
static ABI int32_t is_signal_return(uint64_t pc) { (void)pc; return 0; }
static ABI void pthread_cxa_finalize(void *dso) { (void)dso; }
/* Newer libc.prx (SDK 9) registers its thread-local destructor runner at start-up. Recorded only:
 * guest threads that end do not run it yet. */
GuestCallback runtime_guest_thread_dtors;
static ABI void set_thread_dtors(GuestCallback dtors) { runtime_guest_thread_dtors=dtors; }
/* The same libc also registers its thread atexit counter and report hooks; recorded the same way. */
GuestCallback runtime_guest_thread_atexit_count, runtime_guest_thread_atexit_report;
static ABI void set_thread_atexit_count(GuestCallback count) { runtime_guest_thread_atexit_count=count; }
static ABI void set_thread_atexit_report(GuestCallback report) { runtime_guest_thread_atexit_report=report; }
/* sceKernelIsNeoMode / sceKernelHasNeoMode: 1 on a PS4 Pro, which render_resolution=4k reports
 * (probe.c), as the GPU core does (gpu/shim/theatrhythm_gpu.cpp). */
static int neo;
void runtime_kernel_set_neo_mode(int enabled) { neo=enabled!=0; }
static ABI int32_t neo_mode(void) { return neo; }
/* param.sfo ATTRIBUTE (probe.c). sceKernelGetCpumode: 2 when the title asks for six and seven
 * CPUs (bits 15 and 16), 5 for seven only, else 0 (shadPS4 process.cpp). */
static uint32_t psf_attributes;
void runtime_kernel_set_psf_attributes(uint32_t attributes) { psf_attributes=attributes; }
static ABI int32_t cpu_mode(void) {
    int six=(psf_attributes>>15)&1, seven=(psf_attributes>>16)&1;
    return six && seven ? 2 : seven ? 5 : 0;
}
/* Retail builds run without AddressSanitizer: no replacement allocator tables. */
static ABI void *sanitizer_replace_external(void) { return NULL; }
static ABI int32_t sanitizer_enabled(void) { return 0; }
/* libc.prx's heap asks where the heap trace tables live (as shadPS4: an empty id mask and a
 * state table whose first entry is set); nothing reads them back on the host. */
typedef struct { uint64_t size; uint32_t flags, get_segment_info; uint64_t *mspace_atomic_id_mask, *mstate_table; } HeapTraceInfo;
static uint64_t heap_trace_id_mask, heap_trace_mstate[64]={1};
static ABI int32_t heap_trace_info(HeapTraceInfo *info) {
    info->get_segment_info=0;
    info->mspace_atomic_id_mask=&heap_trace_id_mask;
    info->mstate_table=heap_trace_mstate;
    return 0;
}
static ABI __attribute__((noreturn)) void unwind_resume(void) {
    fputs("STOP: guest C++ exception unwinding (sigreturn/__Ux86_64_setcontext) is not supported\n",stderr);
    runtime_report();
    exit(23);
}
static ABI int32_t print_backtrace(void) {
    fputs("Runtime: guest requested a backtrace (not available)\n",stderr);
    return 0;
}
typedef struct { uint32_t bits[4]; } GuestSigset;
static _Thread_local GuestSigset signal_mask;
static uintptr_t handlers[128];
static ABI uintptr_t guest_signal(int sig,uintptr_t handler) {
    if (sig<=0 || sig>=128) { *runtime_errno()=22; return (uintptr_t)-1; } /* SIG_ERR */
    uintptr_t old=handlers[sig]; handlers[sig]=handler;
    printf("Runtime: guest signal(%d) handler recorded (host signals are not forwarded)\n",sig);
    return old;
}
static ABI int32_t guest_sigprocmask(int how,const GuestSigset *set,GuestSigset *old) {
    if (old) *old=signal_mask;
    if (!set) return 0;
    for (int i=0;i<4;++i) {
        if (how==1) signal_mask.bits[i]|=set->bits[i];        /* SIG_BLOCK */
        else if (how==2) signal_mask.bits[i]&=~set->bits[i];  /* SIG_UNBLOCK */
        else if (how==3) signal_mask.bits[i]=set->bits[i];    /* SIG_SETMASK */
        else return fail_posix(EINVAL);
    }
    return 0;
}
static ABI int32_t guest_sigfillset(GuestSigset *set) { if (!set) return fail_posix(EINVAL); memset(set,0xff,sizeof(*set)); return 0; }
static ABI int32_t guest_sigemptyset(GuestSigset *set) { if (!set) return fail_posix(EINVAL); memset(set,0,sizeof(*set)); return 0; }
typedef struct { GuestTimeval utime, stime; int64_t rest[14]; } GuestRusage;
static ABI int32_t guest_getrusage(int who,GuestRusage *out) {
    if (!out || (who!=0 && who!=1)) return fail_posix(EINVAL);
    memset(out,0,sizeof(*out));
#ifdef _WIN32
    int64_t user=0,kernel=0;
    compat_cpu_times(who,&user,&kernel);
    out->utime=(GuestTimeval){user/1000000,user%1000000};
    out->stime=(GuestTimeval){kernel/1000000,kernel%1000000};
#else
    struct rusage r;
    getrusage(who==0 ? RUSAGE_SELF : RUSAGE_THREAD,&r);
    out->utime=(GuestTimeval){r.ru_utime.tv_sec,r.ru_utime.tv_usec};
    out->stime=(GuestTimeval){r.ru_stime.tv_sec,r.ru_stime.tv_usec};
#endif
    return 0;
}
/* sceKernelUuidCreate(uuid): a random (version 4) UUID in Windows UUID byte order, as shadPS4's
 * UuidCreate; EINVAL without a destination. */
static ABI int32_t uuid_create(unsigned char *uuid) {
    if (!uuid) return (int32_t)0x80020016;
#ifdef _WIN32
    if (compat_random(uuid,16)) return (int32_t)0x8002000e;
#else
    if (getrandom(uuid,16,0)!=16) return (int32_t)0x8002000e;
#endif
    uuid[7]=(unsigned char)((uuid[7]&0x0f)|0x40); /* Data3 high byte: version 4 */
    uuid[8]=(unsigned char)((uuid[8]&0x3f)|0x80); /* RFC 4122 variant */
    return 0;
}
static ABI int32_t guest_sysctl(const int32_t *name,uint32_t namelen,void *old,uint64_t *oldlen,const void *new_value,uint64_t newlen) {
    (void)newlen;
    if (!name || namelen<2 || new_value) return fail_posix(EINVAL);
    if (name[0]==1 && name[1]==37) { /* kern.arandom */
        if (!old || !oldlen) return fail_posix(EINVAL);
#ifdef _WIN32
        if (compat_random(old,(size_t)*oldlen)) return fail_posix(EIO);
#else
        if (getrandom(old,(size_t)*oldlen,0)<0) return fail_posix(errno);
#endif
        return 0;
    }
    if (name[0]==6 && (name[1]==7 || name[1]==3)) { /* hw.pagesize / hw.ncpu */
        if (!oldlen) return fail_posix(EINVAL);
        int32_t value=name[1]==7 ? PAGE : 7;
        if (old) { if (*oldlen<4) return fail_posix(ENOMEM); memcpy(old,&value,4); }
        *oldlen=4; return 0;
    }
    fprintf(stderr,"STOP: unsupported sysctl mib");
    for (uint32_t i=0;i<namelen && i<8;++i) fprintf(stderr," %d",name[i]);
    fputc('\n',stderr);
    exit(21);
}

/* ---- pthread_once and thread-specific data ---- */
static ABI int32_t thread_once(int32_t *once,void (ABI *routine)(void)) {
    if (!once || !routine) return ERR(22);
    for (;;) {
        int32_t state=__atomic_load_n(once,__ATOMIC_ACQUIRE);
        if (state==1) return 0;
        if (state==0 && __atomic_compare_exchange_n(once,&state,2,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            routine();
            __atomic_store_n(once,1,__ATOMIC_RELEASE);
            return 0;
        }
        sched_yield();
    }
}
static ABI int32_t posix_once(int32_t *once,void (ABI *routine)(void)) { return thread_once(once,routine) ? 22 : 0; }
#define KEYS 256
typedef void (ABI *KeyDestructor)(void *);
static struct { int used; KeyDestructor destructor; } keys[KEYS];
static pthread_mutex_t key_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local void *key_values[KEYS];
static ABI int32_t key_create(uint32_t *key,KeyDestructor destructor) {
    if (!key) return ERR(22);
    pthread_mutex_lock(&key_lock);
    for (uint32_t i=1;i<KEYS;++i) if (!keys[i].used) {
        keys[i].used=1; keys[i].destructor=destructor;
        pthread_mutex_unlock(&key_lock);
        *key=i; return 0;
    }
    pthread_mutex_unlock(&key_lock);
    return ERR(35);
}
static ABI int32_t key_delete(uint32_t key) {
    if (!key || key>=KEYS) return ERR(22);
    pthread_mutex_lock(&key_lock);
    int32_t r=keys[key].used ? 0 : ERR(22);
    keys[key].used=0; keys[key].destructor=NULL;
    pthread_mutex_unlock(&key_lock);
    return r;
}
static ABI int32_t key_set(uint32_t key,void *value) {
    if (!key || key>=KEYS || !keys[key].used) return ERR(22);
    key_values[key]=value; return 0;
}
static ABI void *key_get(uint32_t key) { return key && key<KEYS ? key_values[key] : NULL; }
void runtime_thread_keys_cleanup(void) {
    for (int round=0;round<4;++round) {
        int any=0;
        for (uint32_t i=1;i<KEYS;++i) {
            void *value=key_values[i];
            if (!value || !keys[i].used || !keys[i].destructor) continue;
            key_values[i]=NULL; any=1;
            keys[i].destructor(value);
        }
        if (!any) break;
    }
}
static ABI int32_t posix_key_create(uint32_t *k,KeyDestructor d) { int32_t r=key_create(k,d); return r ? r&0xffff : 0; }
static ABI int32_t posix_key_delete(uint32_t k) { int32_t r=key_delete(k); return r ? r&0xffff : 0; }
static ABI int32_t posix_key_set(uint32_t k,void *v) { int32_t r=key_set(k,v); return r ? r&0xffff : 0; }

static const RuntimeExport exports[]={
    {"sceKernelClockGettime",kernel_clock_gettime}, {"clock_gettime",posix_clock_gettime},
    {"clock_getres",posix_clock_getres},
    {"sceKernelGetProcessTime",process_time}, {"sceKernelGetProcessTimeCounter",process_time_counter},
    {"sceKernelGetProcessTimeCounterFrequency",process_time_frequency},
    {"sceKernelReadTsc",read_tsc}, {"sceKernelGetTscFrequency",tsc_frequency},
    {"sceKernelUsleep",kernel_usleep}, {"usleep",posix_usleep}, {"sleep",posix_sleep},
    {"nanosleep",posix_nanosleep}, {"_nanosleep",posix_nanosleep}, {"sceKernelNanosleep",kernel_nanosleep},
    {"sceKernelSleep",kernel_sleep},
    {"sceKernelConvertUtcToLocaltime",kernel_utc_to_local}, {"sceKernelConvertLocaltimeToUtc",kernel_local_to_utc},
    {"chmod",posix_chmod}, {"sceKernelChmod",kernel_chmod}, {"fchmod",posix_fchmod},
    {"utimes",posix_utimes}, {"sceKernelUtimes",kernel_utimes}, {"futimes",posix_futimes},
    {"fcntl",posix_fcntl}, {"sceKernelGetdents",kernel_getdents},
    {"sceKernelSetGPO",kernel_set_gpo}, {"sceKernelInternalMemoryGetModuleSegmentInfo",internal_segment_info},
    {"sceKernelGettimezone",kernel_gettimezone}, {"gettimeofday",posix_gettimeofday},
    {"sceKernelGettimeofday",kernel_gettimeofday},
    {"time",posix_time},
    {"getpagesize",get_pagesize}, {"getpid",get_pid}, {"sched_yield",yield},
    {"_exit",hard_exit},
    {"sceKernelDebugRaiseException",raise_exception},
    {"sceKernelDebugRaiseExceptionOnReleaseMode",raise_exception},
    {"sceKernelPrintBacktraceWithModuleInfo",print_backtrace},
    {"getargc",get_argc}, {"getargv",get_argv}, {"sceKernelStopUnloadModule",stop_unload_module},
    {"sceKernelGetModuleInfoFromAddr",module_info_from_addr}, {"sceKernelMlock",kernel_mlock},
    {"__elf_phdr_match_addr",elf_phdr_match_addr}, {"_is_signal_return",is_signal_return},
    {"__pthread_cxa_finalize",pthread_cxa_finalize}, {"_sceKernelSetThreadDtors",set_thread_dtors},
    {"_sceKernelSetThreadAtexitCount",set_thread_atexit_count}, {"_sceKernelSetThreadAtexitReport",set_thread_atexit_report},
    {"sceKernelGetSanitizerNewReplaceExternal",sanitizer_replace_external},
    {"sceKernelGetSanitizerMallocReplaceExternal",sanitizer_replace_external},
    {"sceKernelIsAddressSanitizerEnabled",sanitizer_enabled}, {"sceLibcHeapGetTraceInfo",heap_trace_info},
    {"sceKernelIsNeoMode",neo_mode}, {"sceKernelHasNeoMode",neo_mode}, {"sceKernelGetCpumode",cpu_mode}, {"sigreturn",unwind_resume},
    {"__Ux86_64_setcontext",unwind_resume}, {"sceKernelUuidCreate",uuid_create},
    {"signal",guest_signal}, {"sigprocmask",guest_sigprocmask}, {"_sigprocmask",guest_sigprocmask},
    {"sigfillset",guest_sigfillset}, {"sigemptyset",guest_sigemptyset},
    {"getrusage",guest_getrusage}, {"sysctl",guest_sysctl},
    {"scePthreadOnce",thread_once}, {"pthread_once",posix_once},
    {"scePthreadKeyCreate",key_create}, {"scePthreadKeyDelete",key_delete},
    {"scePthreadSetspecific",key_set}, {"scePthreadGetspecific",key_get},
    {"pthread_key_create",posix_key_create}, {"pthread_key_delete",posix_key_delete},
    {"pthread_setspecific",posix_key_set}, {"pthread_getspecific",key_get},
};
uintptr_t runtime_kernel_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
