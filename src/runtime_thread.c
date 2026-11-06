/* Guest threads on host pthreads. Each guest thread owns a FreeBSD-style TCB
 * (variant II: static TLS below the TCB). The loader rewrites the eboot's
 * `mov rax, fs:[0]` into `mov rax, gs:[0]`, so GS base = guest TCB while glibc
 * keeps FS. Priorities/affinity are recorded, not enforced by a PS4 scheduler.
 * Windows: GS is the TEB and cannot be moved. The guest TCB lives in a TLS slot of the TEB
 * instead, and the loader rewrites the load to `mov rax, gs:[0x1480+8*slot]` (TlsSlots),
 * the same 9-byte instruction with another displacement. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <asm/prctl.h>
#endif
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define ATTR_MAGIC UINT32_C(0x41545452)
#define STACK_MARGIN (256*1024)
#define MIN_STACK (64*1024)
#define DEFAULT_STACK (1024*1024)
#define DEFAULT_PRIO 700

typedef void *(ABI *GuestEntry)(void *);
typedef struct ThreadAttr {
    uint32_t magic;
    int detached, policy, prio, inherit;
    uint64_t stack, guard, affinity;
    void *stack_address; /* scePthreadAttrSetstack: recorded; the host still places the stack */
    struct ThreadAttr *next;
} ThreadAttr;
typedef struct GuestThread {
    /* Guest-visible TCB is allocated separately; this is the ScePthread handle. */
    uint64_t *tcb;
    unsigned char *tls_block;
    pthread_t host;
    GuestEntry entry;
    void *argument, *result;
    ThreadAttr attr;
    char name[32];
    int detached, finished, joined, host_owned;
    int tid, cancel_disabled, cancel_pending;
    void **cleanup; /* innermost pthread_cleanup_push buffer: {routine, arg, outer} */
    void *stack_low; uint64_t stack_size; /* the host stack, recorded by attach() on the thread */
#ifdef _WIN32
    HANDLE os_handle; /* own duplicate: winpthreads closes a detached thread's handle */
#else
    /* raise_exception's signal: the raiser's number, the number the thread answered, whether it
     * ran the handler (0: it was in host code) and whether it must. */
    uint32_t raise_seq, raise_done;
    int raise_ran, raise_force;
    uint64_t raise_rip, raise_rsp; int raise_still; /* the last "not yet" at a system call */
#endif
#ifdef _WIN32
    theatrhythm_jmp_buf exit_jump; /* no SEH unwinding through guest frames */
#else
    jmp_buf exit_jump;
#endif
    struct GuestThread *next;
} GuestThread;

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static ThreadAttr *attributes;
static GuestThread *threads;
static _Thread_local GuestThread *current;

static int slow_wait_ms(void) {
    static int threshold_ms=-1;
    if (threshold_ms<0) { const char *e=getenv("THEATRHYTHM_SLOW_WAITS"); threshold_ms=e ? atoi(e) : 0; }
    return threshold_ms;
}
uint64_t runtime_wait_begin(void) {
    if (slow_wait_ms()<=0) return 0;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;
}
void runtime_wait_end(const char *what,uint64_t start,const char *detail,long long arg) {
    if (!start) return;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    const uint64_t ns=(uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec-start;
    if (ns<(uint64_t)slow_wait_ms()*1000000ull) return;
    static unsigned logged;
    if (__atomic_fetch_add(&logged,1,__ATOMIC_RELAXED)>=4000) return;
    /* Callers read errno after this (a failed read returns -errno); a failed log write must not
     * change it. */
    const int saved_errno=errno;
    fprintf(stderr,"Slow wait: %s %.1f ms, thread '%s' %s %lld\n",what,ns/1e6,
            current ? current->name : "host",detail ? detail : "",arg);
    errno=saved_errno;
}
static _Thread_local int32_t guest_errno;
static const unsigned char *tls_template;
static uint64_t tls_filesz, tls_memsz, tls_align=16;
static size_t created, joined_count, exited;

void runtime_set_main_tls(const void *data,uint64_t filesz,uint64_t memsz,uint64_t align) {
    tls_template=data; tls_filesz=filesz; tls_memsz=memsz; tls_align=align ? align : 16;
}
static void set_host_name(const char *name);
static uint64_t tls_offset(void) { return (tls_memsz+tls_align-1)&~(tls_align-1); }
#ifdef _WIN32
/* TEB TlsSlots[64] start at gs:0x1480; slots 64+ live elsewhere and cannot be used. */
static DWORD tls_slot=TLS_OUT_OF_INDEXES;
uint32_t runtime_thread_tcb_offset(void) {
    if (tls_slot==TLS_OUT_OF_INDEXES) {
        tls_slot=TlsAlloc();
        if (tls_slot>=64) { fprintf(stderr,"STOP: no TEB TLS slot below 64 (got %lu)\n",tls_slot); exit(21); }
    }
    return 0x1480+8*(uint32_t)tls_slot;
}
static void set_gs(void *base) {
    runtime_thread_tcb_offset();
    if (!TlsSetValue(tls_slot,base)) { fprintf(stderr,"STOP: TlsSetValue failed (%lu)\n",GetLastError()); exit(21); }
}
#else
static void set_gs(void *base) {
    if (syscall(SYS_arch_prctl,ARCH_SET_GS,(unsigned long)base)) { perror("STOP: arch_prctl(ARCH_SET_GS)"); exit(21); }
}
#endif
/* The calling thread's stack: lowest address and size (Windows: TEB DeallocationStack..StackBase). */
static void record_stack(GuestThread *t) {
#ifdef _WIN32
    ULONG_PTR low=0, high=0;
    GetCurrentThreadStackLimits(&low,&high);
    t->stack_low=(void *)low; t->stack_size=high-low;
#else
    pthread_attr_t a;
    size_t size=0;
    if (!pthread_getattr_np(pthread_self(),&a)) { pthread_attr_getstack(&a,&t->stack_low,&size); pthread_attr_destroy(&a); }
    t->stack_size=size;
#endif
}
/* Build TCB/static TLS for the calling host thread and point GS at it. */
static void attach(GuestThread *t) {
    uint64_t offset=tls_offset();
    size_t total=offset+256;
    unsigned char *block=runtime_low_alloc(total);
    if (!block) { fputs("Cannot allocate guest TLS\n",stderr); exit(1); }
    memset(block,0,total);
    if (tls_filesz) memcpy(block,tls_template,tls_filesz);
    uint64_t *tcb=(uint64_t *)(block+offset);
    static uint64_t dtv[3];
    tcb[0]=(uint64_t)(uintptr_t)tcb;         /* tcb_self */
    tcb[1]=(uint64_t)(uintptr_t)dtv;         /* tcb_dtv (static module only) */
    tcb[2]=(uint64_t)(uintptr_t)t;           /* tcb_thread */
    t->tls_block=block; t->tcb=tcb;
    record_stack(t);
#ifdef _WIN32
    DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&t->os_handle,0,FALSE,DUPLICATE_SAME_ACCESS);
#endif
    set_gs(tcb);
    current=t;
}
static GuestThread *new_thread(void) {
    static int next_tid;
    GuestThread *t=runtime_low_alloc(sizeof(*t));
    if (!t) return NULL;
    t->attr=(ThreadAttr){.magic=ATTR_MAGIC,.policy=1,.prio=DEFAULT_PRIO,.stack=DEFAULT_STACK,.affinity=0x7f};
    t->tid=__atomic_add_fetch(&next_tid,1,__ATOMIC_RELAXED); /* as shadPS4: a counter from 1 */
    return t;
}
static void publish(GuestThread *t) {
    pthread_mutex_lock(&lock); t->next=threads; threads=t; pthread_mutex_unlock(&lock);
}
/* Host threads that call into guest code without being created by the guest
 * (the main thread, test threads) get a record on first use. */
GuestThread *runtime_thread_current(void) {
    if (current) return current;
    GuestThread *t=new_thread();
    if (!t) { fputs("Cannot allocate guest thread\n",stderr); exit(1); }
    t->host=pthread_self(); t->host_owned=1;
    snprintf(t->name,sizeof(t->name),"host");
    attach(t); publish(t);
    return t;
}
/* Host threads that call guest code (HLE decoders invoking guest callbacks) need
 * a TCB/TLS like guest threads; the record lives until process exit. */
void runtime_thread_attach_host(const char *name) {
    GuestThread *t=runtime_thread_current();
    snprintf(t->name,sizeof(t->name),"%s",name ? name : "host");
}
void runtime_thread_attach_main(void) {
    GuestThread *t=runtime_thread_current();
    snprintf(t->name,sizeof(t->name),"main");
    set_host_name(t->name);
}
void runtime_thread_set_stack(void *low,uint64_t size) {
    GuestThread *t=runtime_thread_current();
    t->stack_low=low; t->stack_size=size;
}
/* Records are never removed (games create threads again during every load), so a found
 * thread moves to the front: the threads the game asks about (priorities, affinity) are not
 * behind every finished one (from Awakenchan 0.9.34 beta 9.9). THEATRHYTHM_THREAD_FRONT=0: the list
 * keeps its order, as before. */
static GuestThread *find_thread(void *handle) {
    static int front=-1;
    if (front<0) { const char *e=getenv("THEATRHYTHM_THREAD_FRONT"); front=!(e && e[0]=='0'); }
    GuestThread *found=NULL;
    pthread_mutex_lock(&lock);
    GuestThread **link=&threads;
    for (GuestThread *t=threads;t;link=&t->next,t=t->next) {
        if (t!=handle) continue;
        found=t;
        if (front && link!=&threads) { *link=t->next; t->next=threads; threads=t; }
        break;
    }
    pthread_mutex_unlock(&lock);
    return found;
}
static ThreadAttr *find_attr(ThreadAttr **slot) {
    if (!slot || !*slot) return NULL;
    ThreadAttr *found=NULL;
    pthread_mutex_lock(&lock);
    for (ThreadAttr *a=attributes;a;a=a->next) if (a==*slot && a->magic==ATTR_MAGIC) { found=a; break; }
    pthread_mutex_unlock(&lock);
    return found;
}

static ABI void *thread_self(void) { return runtime_thread_current(); }
static ABI int32_t *guest_error(void) { return &guest_errno; }
int32_t *runtime_errno(void) { return &guest_errno; }

static ABI int32_t attr_init(ThreadAttr **out) {
    if (!out) return ERR(22);
    ThreadAttr *a=runtime_low_alloc(sizeof(*a));
    if (!a) return ERR(12);
    *a=(ThreadAttr){.magic=ATTR_MAGIC,.policy=1,.prio=DEFAULT_PRIO,.inherit=4,.stack=DEFAULT_STACK,.guard=4096,.affinity=0x7f};
    pthread_mutex_lock(&lock); a->next=attributes; attributes=a; pthread_mutex_unlock(&lock);
    *out=a; return 0;
}
static ABI int32_t attr_destroy(ThreadAttr **slot) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    pthread_mutex_lock(&lock);
    ThreadAttr **link=&attributes;
    while (*link!=a) link=&(*link)->next;
    *link=a->next; a->magic=0;
    pthread_mutex_unlock(&lock);
    runtime_low_free(a); *slot=NULL; return 0;
}
static ABI int32_t attr_get(void *thread,ThreadAttr **out) {
    ThreadAttr *a=find_attr(out);
    if (!a || !thread) return ERR(22);
    GuestThread *t=find_thread(thread);
    if (!t) return ERR(3);
    ThreadAttr *next=a->next;
    *a=t->attr; a->magic=ATTR_MAGIC; a->next=next;
    a->detached=t->detached;
    /* The stack the thread really runs on, as shadPS4 reports the stack it allocated: the IL2CPP
     * collector scans from a thread's stack pointer up to stackaddr + stacksize. */
    if (t->stack_low) { a->stack_address=t->stack_low; a->stack=t->stack_size; }
    return 0;
}
static ABI int32_t attr_set_stack(ThreadAttr **slot,uint64_t size) {
    ThreadAttr *a=find_attr(slot);
    if (!a || size<16384) return ERR(22);
    a->stack=size; return 0;
}
static ABI int32_t attr_get_stack(ThreadAttr **slot,uint64_t *size) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !size) return ERR(22);
    *size=a->stack; return 0;
}
static ABI int32_t attr_set_detach(ThreadAttr **slot,int state) {
    ThreadAttr *a=find_attr(slot);
    if (!a || (state!=0 && state!=1)) return ERR(22);
    a->detached=state; return 0;
}
static ABI int32_t attr_get_detach(ThreadAttr **slot,int *state) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !state) return ERR(22);
    *state=a->detached; return 0;
}
static ABI int32_t attr_set_policy(ThreadAttr **slot,int policy) {
    ThreadAttr *a=find_attr(slot);
    if (!a || policy<1 || policy>3) return ERR(22);
    a->policy=policy; return 0;
}
static ABI int32_t attr_set_inherit(ThreadAttr **slot,int inherit) {
    ThreadAttr *a=find_attr(slot);
    if (!a || (inherit!=0 && inherit!=4)) return ERR(22);
    a->inherit=inherit; return 0;
}
static ABI int32_t attr_set_param(ThreadAttr **slot,const int *param) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !param) return ERR(22);
    a->prio=*param; return 0;
}
static ABI int32_t attr_get_param(ThreadAttr **slot,int *param) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !param) return ERR(22);
    *param=a->prio; return 0;
}
static ABI int32_t attr_set_affinity(ThreadAttr **slot,uint64_t mask) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    a->affinity=mask; return 0;
}
static ABI int32_t attr_affinity(ThreadAttr **slot,uint64_t *mask) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !mask) return ERR(22);
    *mask=a->affinity; return 0;
}
static ABI int32_t attr_set_guard(ThreadAttr **slot,uint64_t size) {
    ThreadAttr *a=find_attr(slot);
    if (!a) return ERR(22);
    a->guard=size; return 0;
}
/* scePthreadAttrSetstack(attr, address, size): both recorded and returned by the getters; a
 * thread made from the attribute gets a host stack of that size (the address is not used). */
static ABI int32_t attr_set_stack_area(ThreadAttr **slot,void *address,uint64_t size) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !address || size<16384) return ERR(22);
    a->stack_address=address; a->stack=size; return 0;
}
static ABI int32_t attr_get_stack_area(ThreadAttr **slot,void **address,uint64_t *size) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !address || !size) return ERR(22);
    *address=a->stack_address; *size=a->stack; return 0;
}
static ABI int32_t attr_get_stack_address(ThreadAttr **slot,void **address) {
    ThreadAttr *a=find_attr(slot);
    if (!a || !address) return ERR(22);
    *address=a->stack_address; return 0;
}

/* Linux thread names hold 15 characters; longer ones would be rejected. */
static void set_host_name(const char *name) {
    char host[16]={0};
    memcpy(host,name,strnlen(name,sizeof(host)-1));
    pthread_setname_np(pthread_self(),host);
#ifdef _WIN32
    /* winpthreads only names the thread for an attached debugger; the CPU report reads this. */
    wchar_t wide[32];
    if (MultiByteToWideChar(CP_UTF8,0,name,-1,wide,32)) SetThreadDescription(GetCurrentThread(),wide);
#endif
}
/* The game's job workers take their queue with a bare spinlock (0x5a57f0: lock cmpxchg, no pause
 * or yield). On a 4-thread host the holder gets preempted and the others spin whole quanta,
 * starving the GPU command processor (GPU 17% -> 72% with the workers below normal).
 * The game's own scePthreadSetprio (it raises them to 256) stays recorded only.
 * Threads the game creates with a high PS4 priority (below 400: brbDriver, the sound driver at 306,
 * and Fios I/O) run above normal, or the audio output starves on 4 threads (48 ms gaps). */
enum { HIGH_GUEST_PRIO = 400 };
static void set_host_priority(const char *name,int guest_prio) {
    int lower=!strncmp(name,"WorkerThread",12);
    if (!lower && guest_prio>=HIGH_GUEST_PRIO) return;
#ifdef _WIN32
    if (!SetThreadPriority(GetCurrentThread(),lower ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_ABOVE_NORMAL))
        printf("Runtime: cannot change the priority of '%s' (%lu)\n",name,GetLastError());
#else
    /* Raising needs CAP_SYS_NICE; without it the thread keeps the normal priority. */
    if (setpriority(PRIO_PROCESS,(id_t)syscall(SYS_gettid),lower ? 5 : -5) && lower)
        printf("Runtime: cannot change the priority of '%s' (%d)\n",name,errno);
#endif
}
#ifdef _WIN32
#define exit_setjmp(t) theatrhythm_setjmp((t)->exit_jump)
#define exit_longjmp(t) theatrhythm_longjmp((t)->exit_jump,1)
#else
#define exit_setjmp(t) setjmp((t)->exit_jump)
#define exit_longjmp(t) longjmp((t)->exit_jump,1)
#endif
/* The PS4 runs its threads with flush-to-zero and denormals-are-zero set in MXCSR, and the x87
 * control word Linux also uses; a Linux or Windows thread starts with both flags clear (MXCSR
 * 0x1f80). Game code gets different results with them clear (very small values stay as denormals
 * instead of becoming 0, and a physics step can drift or blow up from there) and runs slower on
 * denormals. The values are the ones dumped from real hardware, as in shadPS4 (shadPS4#3568,
 * StevenMiller123, found by red-prig): set on the game's main thread before its first code, and
 * on each thread it creates. */
void runtime_orbis_fpu(void) {
#if defined(__x86_64__)
    const uint32_t mxcsr=0x9fc0;
    const uint16_t fpucw=0x037f;
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
    __asm__ volatile("fldcw %0" : : "m"(fpucw));
#endif
}
static void *host_start(void *p) {
    GuestThread *t=p;
    runtime_orbis_fpu();
    attach(t);
    set_host_name(t->name);
    set_host_priority(t->name,t->attr.prio);
    if (!exit_setjmp(t)) t->result=t->entry(t->argument);
    runtime_thread_keys_cleanup();
    pthread_mutex_lock(&lock); t->finished=1; ++exited;
#ifdef _WIN32
    if (t->os_handle) { CloseHandle(t->os_handle); t->os_handle=NULL; }
#endif
    pthread_mutex_unlock(&lock);
    return t->result;
}
static int32_t create(GuestThread **out,ThreadAttr **attr_slot,GuestEntry entry,void *argument,const char *name) {
    if (!out || !entry) return ERR(22);
    ThreadAttr *a=NULL;
    if (attr_slot && !(a=find_attr(attr_slot))) return ERR(22);
    GuestThread *t=new_thread();
    if (!t) return ERR(12);
    if (a) { t->attr=*a; t->attr.next=NULL; }
    t->entry=entry; t->argument=argument; t->detached=t->attr.detached;
    snprintf(t->name,sizeof(t->name),"%s",name ? name : "guest");
    pthread_attr_t host;
    pthread_attr_init(&host);
    uint64_t stack=t->attr.stack<MIN_STACK ? MIN_STACK : t->attr.stack;
    /* Stacks below 1 TiB as on PS4; guest code may pack stack addresses. */
    size_t stack_bytes=(size_t)stack+STACK_MARGIN;
#ifdef _WIN32
    /* Windows places the stack itself (low: no high-entropy ASLR) and commits all of it:
     * guest code has no stack probes and would step over the guard page. */
    pthread_attr_setstacksize(&host,stack_bytes);
#else
    void *stack_memory=runtime_low_map(stack_bytes,PROT_READ|PROT_WRITE);
    if (stack_memory) pthread_attr_setstack(&host,stack_memory,stack_bytes);
    else pthread_attr_setstacksize(&host,stack_bytes);
#endif
    publish(t);
    /* Publish the handle before the thread can run and inspect itself. */
    *out=t;
    int e=pthread_create(&t->host,&host,host_start,t);
    pthread_attr_destroy(&host);
    if (e) { fprintf(stderr,"STOP: host pthread_create failed: %d\n",e); exit(21); }
    if (t->detached) pthread_detach(t->host);
    pthread_mutex_lock(&lock); ++created; pthread_mutex_unlock(&lock);
    if (runtime_verbose()) printf("Runtime: guest thread '%s' created (stack=%llu, prio=%d)\n",t->name,(unsigned long long)stack,t->attr.prio);
    return 0;
}
static ABI int32_t thread_create(GuestThread **out,ThreadAttr **attr,GuestEntry entry,void *argument,const char *name) {
    return create(out,attr,entry,argument,name);
}
static ABI int32_t thread_join(GuestThread *t,void **result) {
    if (!find_thread(t) || t->host_owned) return ERR(3);
    if (t==current) return ERR(11);
    if (t->detached || t->joined) return ERR(22);
    t->joined=1;
    void *value=NULL;
    const uint64_t wait_start=runtime_wait_begin();
    int e=pthread_join(t->host,&value);
    runtime_wait_end("thread join",wait_start,t->name,0);
    if (e) { fprintf(stderr,"STOP: host pthread_join failed: %d\n",e); exit(21); }
    if (result) *result=t->result;
    pthread_mutex_lock(&lock); ++joined_count; pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t thread_detach(GuestThread *t) {
    if (!find_thread(t)) return ERR(3);
    if (t->detached) return ERR(22);
    t->detached=1;
    if (!t->host_owned) pthread_detach(t->host);
    return 0;
}
static ABI __attribute__((noreturn)) void thread_exit(void *value) {
    GuestThread *t=runtime_thread_current();
    if (t->host_owned) { fputs("STOP: pthread_exit on host-owned/main thread\n",stderr); exit(21); }
    t->result=value;
    /* Cleanup handlers still pushed run innermost first (POSIX pthread_exit). */
    while (t->cleanup) {
        void **c=t->cleanup; t->cleanup=c[2];
        ((void (ABI *)(void *))c[0])(c[1]);
    }
    /* No host unwinder: guest frames have no registered FDEs. */
    exit_longjmp(t);
}
/* __pthread_cleanup_push_imp(routine, arg, info) / __pthread_cleanup_pop_imp(execute)
 * (shadPS4 pthread_clean.cpp): pthread_cleanup_push/pop expand to these with a 64-byte
 * struct _pthread_cleanup_info on the caller's stack, which holds {routine, arg, outer} here.
 * Pop removes the innermost handler and runs it when execute is nonzero. */
static ABI void cleanup_push(void *routine,void *arg,void **info) {
    GuestThread *t=runtime_thread_current();
    info[0]=routine; info[1]=arg; info[2]=t->cleanup; t->cleanup=info;
}
static ABI void cleanup_pop(int execute) {
    GuestThread *t=runtime_thread_current();
    void **c=t->cleanup;
    if (!c) return;
    t->cleanup=c[2];
    if (execute) ((void (ABI *)(void *))c[0])(c[1]);
}
/* pthread_cancel(thread): ESRCH for a thread that is not running. With cancellation disabled the
 * request stays pending (pthread_setcancelstate), which is all POSIX asks then; acting on a
 * cancellation needs cancellation points this runtime does not have, so that stops. */
static ABI int32_t posix_cancel(GuestThread *t) {
    GuestThread *found=find_thread(t);
    if (!found || __atomic_load_n(&found->finished,__ATOMIC_ACQUIRE)) return 3;
    if (found->cancel_disabled) { found->cancel_pending=1; return 0; }
    fprintf(stderr,"STOP: pthread_cancel of running thread %s (cancellation is not delivered)\n",found->name);
    exit(21);
}
static ABI int32_t thread_yield(void) { runtime_thread_count_yield(0); sched_yield(); return 0; }
static ABI int32_t thread_get_prio(GuestThread *t,int *prio) {
    if (!find_thread(t)) return ERR(3);
    if (!prio) return ERR(22);
    *prio=t->attr.prio; return 0;
}
static ABI int32_t thread_set_prio(GuestThread *t,int prio) {
    if (!find_thread(t)) return ERR(3);
    t->attr.prio=prio; return 0;
}
static ABI int32_t thread_set_affinity(GuestThread *t,uint64_t mask) {
    if (!find_thread(t)) return ERR(3);
    t->attr.affinity=mask; return 0;
}
static ABI int32_t thread_get_affinity(GuestThread *t,uint64_t *mask) {
    if (!find_thread(t)) return ERR(3);
    if (!mask) return ERR(22);
    *mask=t->attr.affinity; return 0;
}
static ABI int32_t thread_rename(GuestThread *t,const char *name) {
    if (!find_thread(t)) return ERR(3);
    if (!name) return ERR(22);
    snprintf(t->name,sizeof(t->name),"%s",name);
    if (t==current) { set_host_name(t->name); set_host_priority(t->name,t->attr.prio); }
    return 0;
}
static ABI int32_t thread_equal(GuestThread *a,GuestThread *b) { return a==b; }
/* scePthreadGetschedparam(thread, policy, param) / Setschedparam(thread, policy, param): the
 * policy and priority of a live thread (shadPS4 pthread.cpp). Recorded, not enforced, like
 * scePthreadSetprio. A thread that has exited is ESRCH. */
static GuestThread *live_thread(GuestThread *t) {
    GuestThread *found=find_thread(t);
    return found && !__atomic_load_n(&found->finished,__ATOMIC_ACQUIRE) ? found : NULL;
}
static ABI int32_t thread_get_sched(GuestThread *t,int *policy,int *param) {
    if (!policy || !param) return ERR(22);
    if (!live_thread(t)) return ERR(3);
    *policy=t->attr.policy; *param=t->attr.prio; return 0;
}
static ABI int32_t thread_set_sched(GuestThread *t,int policy,const int *param) {
    if (!t || !param) return ERR(22);
    if (!live_thread(t)) return ERR(3);
    t->attr.policy=policy; t->attr.prio=*param; return 0;
}
/* scePthreadGetname(thread, name): the thread's name in a 32-byte buffer. */
static ABI int32_t thread_get_name(GuestThread *t,char *name) {
    if (!name) return ERR(22);
    if (!find_thread(t)) return ERR(3);
    memcpy(name,t->name,sizeof(t->name)); return 0;
}
/* scePthreadGetthreadid: the calling thread's id, a positive counter. */
static ABI int32_t thread_get_id(void) { return runtime_thread_current()->tid; }
/* sched_get_priority_max/min(policy): the FIFO range 767..256 for FIFO and round-robin; any
 * other policy returns EINVAL (22) itself, as shadPS4 does. */
static ABI int32_t priority_max(int policy) { return policy==1 || policy==3 ? 767 : 22; }
static ABI int32_t priority_min(int policy) { return policy==1 || policy==3 ? 256 : 22; }
/* pthread_setcancelstate(state, old): 0 enables, 1 disables. Recorded per thread; enabling it
 * with a cancellation pending (posix_cancel) would have to act on it, which stops. */
static ABI int32_t set_cancel_state(int state,int *old) {
    if (state!=0 && state!=1) return 22;
    GuestThread *t=runtime_thread_current();
    if (old) *old=t->cancel_disabled;
    t->cancel_disabled=state;
    if (!state && t->cancel_pending) {
        fprintf(stderr,"STOP: pending pthread_cancel of thread %s enabled (cancellation is not delivered)\n",t->name);
        exit(21);
    }
    return 0;
}
/* libc counts the thread-exit destructors a module registers so it is not unloaded while they
 * are pending; modules are never unloaded here, so the count is kept for the report only. */
static int64_t rtld_atexit_pending;
static ABI void rtld_atexit_increment(void) { __atomic_add_fetch(&rtld_atexit_pending,1,__ATOMIC_RELAXED); }
static ABI void rtld_atexit_decrement(void) { __atomic_sub_fetch(&rtld_atexit_pending,1,__ATOMIC_RELAXED); }

/* ---- Exception handlers (shadPS4 threads/exception.cpp) ----
 * sceKernelInstallExceptionHandler(signum, handler) accepts SIGHUP, SIGILL, SIGFPE, SIGBUS,
 * SIGSEGV and SIGUSR1 (EINVAL otherwise) and refuses a second handler (EAGAIN);
 * sceKernelRemoveExceptionHandler(signum) clears it. sceKernelRaiseException(thread, SIGUSR1)
 * runs the handler on that thread as handler(signum, ucontext*): the IL2CPP collector suspends
 * threads this way and reads the stack pointer from the context (PS4Util.prx). Windows: a thread
 * running guest code is suspended and resumed at redzone_deliver, which runs the handler on it
 * below its red zone (no APC: Wine places those differently). One running host code is let go
 * until it is back in guest code or blocked, since it may hold a lock the handler takes (the
 * handler then waited on an event flag while its own thread held their lock: a black screen). A
 * system call counts as host code unless it is a wait. A thread blocked in a wait stays suspended
 * while a proxy host thread, with the target's thread record and TCB, runs the handler, and resumes
 * when it returns; register changes are not written back there.
 * Linux: a host signal runs it on the thread itself, in guest code or blocked in a wait; one that
 * lands in other host code (a system call returning included) is answered "not yet" and sent
 * again, for the same reason.
 * The handler sees a stack pointer below the saved registers, so a conservative stack scan from
 * it covers them as well. The guest signal mask is not consulted; host faults never reach here. */
typedef struct {
    uint64_t onstack, rdi, rsi, rdx, rcx, r8, r9, rax, rbx, rbp, r10, r11, r12, r13, r14, r15;
    int32_t trapno; uint16_t fs, gs;
    uint64_t addr; int32_t flags; uint16_t es, ds;
    uint64_t err, rip, cs, rflags, rsp, ss, len, fpformat, ownedfp, lbrfrom, lbrto, aux1, aux2;
    uint64_t fpstate[104], fsbase, gsbase, spare[6];
} GuestMcontext;
typedef struct {
    uint32_t sigmask[4]; int32_t pad[12];
    GuestMcontext mc;
    void *link, *stack_sp; uint64_t stack_size; int32_t stack_flags, stack_align;
    int32_t flags, spare[4], tail[3];
} GuestUcontext;
_Static_assert(__builtin_offsetof(GuestUcontext,mc.rsp)==0xf8,"FreeBSD ucontext layout");
typedef void (ABI *ExceptionHandler)(int32_t,GuestUcontext *);
static ExceptionHandler exception_handlers[128];
/* About two seconds of retries before a thread still in host code gets the handler there anyway. */
#define RAISE_TRIES 2000
static uint64_t exceptions_raised, exceptions_delivered, exceptions_entered, raised_running, raised_proxy;
static int exception_signal(int32_t signum) {
    return signum==1 || signum==4 || signum==8 || signum==10 || signum==11 || signum==30;
}
#ifdef _WIN32
static void find_wait_calls(void);
#else
static void install_host_signal(void);
#endif
static ABI int32_t install_exception_handler(int32_t signum,ExceptionHandler handler) {
    if (!exception_signal(signum)) return ERR(22);
#ifdef _WIN32
    find_wait_calls();
#else
    install_host_signal();
#endif
    ExceptionHandler none=NULL;
    if (!__atomic_compare_exchange_n(&exception_handlers[signum-1],&none,handler,0,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))
        return ERR(35);
    printf("Runtime: exception handler for signal %d installed\n",signum);
    return 0;
}
static ABI int32_t remove_exception_handler(int32_t signum) {
    if (!exception_signal(signum)) return ERR(22);
    __atomic_store_n(&exception_handlers[signum-1],NULL,__ATOMIC_RELEASE);
    return 0;
}
#ifdef _WIN32
static void context_to_guest(const CONTEXT *c,GuestMcontext *m) {
    m->r8=c->R8; m->r9=c->R9; m->r10=c->R10; m->r11=c->R11; m->r12=c->R12; m->r13=c->R13;
    m->r14=c->R14; m->r15=c->R15; m->rdi=c->Rdi; m->rsi=c->Rsi; m->rbp=c->Rbp; m->rbx=c->Rbx;
    m->rdx=c->Rdx; m->rax=c->Rax; m->rcx=c->Rcx; m->rsp=c->Rsp; m->rip=c->Rip;
    m->rflags=c->EFlags; m->fs=c->SegFs; m->gs=c->SegGs;
}
/* SysV leaf functions keep locals in the 128 bytes below rsp (the red zone), and a frame the OS
 * builds for an APC there clobbers them: FMOD's Vorbis PCM output then read a null pointer (guest
 * 0x17433fc). Windows can be told where the APC runs, Wine (Proton) still clobbered it, so no APC
 * is used: a thread interrupted in guest code is moved to redzone_deliver with rsp RESUME_GAP below
 * its own, its rip and rsp in the two slots there and the signal number below them. It saves every
 * register, flag and the x87/SSE/AVX state (xsave), runs the handler (redzone_handler) with the
 * direction flag clear (the thread may be inside a backward copy), restores them and leaves through
 * redzone_resume, which puts rsp back and jumps to the rip slot. */
#define RESUME_GAP 144
void redzone_resume(void), redzone_resume_jump(void), redzone_deliver(void);
void redzone_handler(uint64_t *saved);
__asm__(".text\n.globl redzone_resume\nredzone_resume:\n"
        " mov 8(%rsp),%rsp\n"
        ".globl redzone_resume_jump\nredzone_resume_jump:\n"
        " jmp *-144(%rsp)\n"
        ".globl redzone_deliver\nredzone_deliver:\n"
        " pushfq\n push %rax\n push %rcx\n push %rdx\n push %rbx\n push %rbp\n push %rsi\n push %rdi\n"
        " push %r8\n push %r9\n push %r10\n push %r11\n push %r12\n push %r13\n push %r14\n push %r15\n"
        " cld\n mov %rsp,%rbx\n sub $1024,%rsp\n and $-64,%rsp\n"
        " xor %eax,%eax\n mov %rax,512(%rsp)\n mov %rax,520(%rsp)\n mov %rax,528(%rsp)\n mov %rax,536(%rsp)\n"
        " mov %rax,544(%rsp)\n mov %rax,552(%rsp)\n mov %rax,560(%rsp)\n mov %rax,568(%rsp)\n"
        " mov $7,%eax\n xor %edx,%edx\n xsave64 (%rsp)\n"
        " mov %rbx,%rcx\n sub $32,%rsp\n call redzone_handler\n add $32,%rsp\n"
        " mov $7,%eax\n xor %edx,%edx\n xrstor64 (%rsp)\n mov %rbx,%rsp\n"
        " pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %r11\n pop %r10\n pop %r9\n pop %r8\n"
        " pop %rdi\n pop %rsi\n pop %rbp\n pop %rbx\n pop %rdx\n pop %rcx\n pop %rax\n popfq\n"
        " lea 8(%rsp),%rsp\n jmp redzone_resume\n");
/* What redzone_deliver pushed, from its stack pointer up, then the signal number and the slots. */
enum { SAVED_R15, SAVED_R14, SAVED_R13, SAVED_R12, SAVED_R11, SAVED_R10, SAVED_R9, SAVED_R8,
       SAVED_RDI, SAVED_RSI, SAVED_RBP, SAVED_RBX, SAVED_RDX, SAVED_RCX, SAVED_RAX, SAVED_RFLAGS,
       SAVED_SIGNUM, SAVED_RIP, SAVED_RSP };
/* A thread stopped inside redzone_resume: the rip and rsp it is on its way back to. */
static void leave_resume(CONTEXT *c) {
    if (c->Rip==(DWORD64)(uintptr_t)redzone_resume) {
        const uint64_t *slot=(const uint64_t *)(uintptr_t)c->Rsp;
        c->Rip=slot[0]; c->Rsp=slot[1];
    } else if (c->Rip==(DWORD64)(uintptr_t)redzone_resume_jump) {
        c->Rip=*(const uint64_t *)(uintptr_t)(c->Rsp-RESUME_GAP);
    }
}
/* Host code (the runner, its DLLs, Windows) is mapped as executable images; the game image, its
 * rewrite stubs and the hook stubs are not. */
static int host_code(DWORD64 rip) {
    MEMORY_BASIC_INFORMATION m;
    return VirtualQuery((const void *)(uintptr_t)rip,&m,sizeof(m)) && m.Type==MEM_IMAGE;
}
/* A thread suspended in a system call has the call's number in rax, whether it is blocked or
 * finishing a quick one (an NtSetEvent under a runtime lock). These are the waits, read from the
 * ntdll stubs (mov r10,rcx; mov eax,N), whose numbers differ between Windows builds and Wine.
 * Read when the handler is installed: nothing is suspended then (one may hold the loader lock). */
static uint32_t wait_calls[8];
static int wait_call_count;
static void find_wait_calls(void) {
    static const char *const names[]={"NtWaitForSingleObject","NtWaitForMultipleObjects","NtDelayExecution",
        "NtWaitForAlertByThreadId","NtSignalAndWaitForSingleObject","NtRemoveIoCompletion",
        "NtRemoveIoCompletionEx","NtWaitForKeyedEvent"};
    HMODULE ntdll=GetModuleHandleA("ntdll.dll");
    if (!ntdll || __atomic_load_n(&wait_call_count,__ATOMIC_ACQUIRE)) return;
    int n=0;
    for (size_t i=0;i<sizeof(names)/sizeof(names[0]);++i) {
        const unsigned char *stub=(const unsigned char *)(uintptr_t)GetProcAddress(ntdll,names[i]);
        if (stub && stub[0]==0x4c && stub[1]==0x8b && stub[2]==0xd1 && stub[3]==0xb8) memcpy(&wait_calls[n++],stub+4,4);
    }
    __atomic_store_n(&wait_call_count,n,__ATOMIC_RELEASE);
}
static int wait_call(DWORD64 rax) {
    for (int i=0,n=__atomic_load_n(&wait_call_count,__ATOMIC_ACQUIRE);i<n;++i)
        if ((uint32_t)rax==wait_calls[i]) return 1;
    return 0;
}
/* Runs on the interrupted thread from redzone_deliver; the reported stack pointer is this frame's
 * context copy, and registers the handler changes are written back on return (rip and rsp through
 * the slots). */
void redzone_handler(uint64_t *saved) {
    int32_t signum=(int32_t)saved[SAVED_SIGNUM];
    ExceptionHandler handler=__atomic_load_n(&exception_handlers[signum-1],__ATOMIC_ACQUIRE);
    if (!handler) return;
    GuestUcontext u={0};
    GuestMcontext *m=&u.mc;
    m->r8=saved[SAVED_R8]; m->r9=saved[SAVED_R9]; m->r10=saved[SAVED_R10]; m->r11=saved[SAVED_R11];
    m->r12=saved[SAVED_R12]; m->r13=saved[SAVED_R13]; m->r14=saved[SAVED_R14]; m->r15=saved[SAVED_R15];
    m->rdi=saved[SAVED_RDI]; m->rsi=saved[SAVED_RSI]; m->rbp=saved[SAVED_RBP]; m->rbx=saved[SAVED_RBX];
    m->rdx=saved[SAVED_RDX]; m->rax=saved[SAVED_RAX]; m->rcx=saved[SAVED_RCX];
    m->rip=saved[SAVED_RIP]; m->rflags=saved[SAVED_RFLAGS];
    __asm__("mov %%fs,%0\n mov %%gs,%1" : "=r"(m->fs), "=r"(m->gs));
    const uint64_t reported=(uint64_t)(uintptr_t)&u;
    m->rsp=reported;
    __atomic_add_fetch(&exceptions_entered,1,__ATOMIC_RELAXED);
    handler(signum,&u);
    saved[SAVED_R8]=m->r8; saved[SAVED_R9]=m->r9; saved[SAVED_R10]=m->r10; saved[SAVED_R11]=m->r11;
    saved[SAVED_R12]=m->r12; saved[SAVED_R13]=m->r13; saved[SAVED_R14]=m->r14; saved[SAVED_R15]=m->r15;
    saved[SAVED_RDI]=m->rdi; saved[SAVED_RSI]=m->rsi; saved[SAVED_RBP]=m->rbp; saved[SAVED_RBX]=m->rbx;
    saved[SAVED_RDX]=m->rdx; saved[SAVED_RAX]=m->rax; saved[SAVED_RCX]=m->rcx;
    saved[SAVED_RIP]=m->rip;
    if (m->rsp!=reported) saved[SAVED_RSP]=m->rsp;
    __atomic_add_fetch(&exceptions_delivered,1,__ATOMIC_RELAXED);
}
/* Raised on the calling thread: the handler runs before the raise returns, with this thread's
 * context; register changes have nowhere to go back to (it is inside the runtime). */
static void deliver_to_self(int32_t signum) {
    ExceptionHandler handler=__atomic_load_n(&exception_handlers[signum-1],__ATOMIC_ACQUIRE);
    if (!handler) return;
    CONTEXT c;
    RtlCaptureContext(&c);
    GuestUcontext u={0};
    context_to_guest(&c,&u.mc);
    u.mc.rsp=(uint64_t)(uintptr_t)&u;
    __atomic_add_fetch(&exceptions_entered,1,__ATOMIC_RELAXED);
    handler(signum,&u);
    __atomic_add_fetch(&exceptions_delivered,1,__ATOMIC_RELAXED);
}
/* Proxy path: the target is suspended in a system call. Its registers are copied below its red
 * zone (it does not touch that stack until resumed), and that address is the reported pointer. */
typedef struct { CONTEXT c; GuestThread *t; HANDLE h; int32_t signum; } Proxy;
static void *proxy_deliver(void *p) {
    Proxy *x=p;
    runtime_orbis_fpu();
    current=x->t; set_gs(x->t->tcb);
    ExceptionHandler handler=__atomic_load_n(&exception_handlers[x->signum-1],__ATOMIC_ACQUIRE);
    if (handler) {
        GuestUcontext u={0};
        context_to_guest(&x->c,&u.mc);
        uint64_t *save=(uint64_t *)(((uintptr_t)x->c.Rsp-128-15*8)&~(uintptr_t)15);
        memcpy(save,&u.mc.rdi,15*8); /* rdi..r15 */
        u.mc.rsp=(uint64_t)(uintptr_t)save;
        __atomic_add_fetch(&exceptions_entered,1,__ATOMIC_RELAXED);
        handler(x->signum,&u);
        __atomic_add_fetch(&exceptions_delivered,1,__ATOMIC_RELAXED);
    }
    ResumeThread(x->h);
    free(x);
    return NULL;
}
#else
/* A real-time signal, which the kernel queues: raises sent back to back are each delivered (a
 * standard signal would merge them). The kernel builds its frame below the red zone, restores every
 * register and the FPU state on return, and restarts or wakes a wait the thread was blocked in. */
#define EXCEPTION_HOST_SIGNAL (SIGRTMIN+2)
/* Host code: this executable's text (not PIE: it sits low) and everything above 1 TiB (shared
 * libraries, the vDSO). The game image, its rewrite stubs and the hook stubs are all lower. */
extern const char __executable_start[], etext[];
static int host_code(uint64_t rip) {
    return (rip>=(uintptr_t)__executable_start && rip<(uintptr_t)etext) || rip>=(UINT64_C(1)<<40);
}
/* A system call that blocks: rax holds its number while it is about to run or will be restarted. */
static int wait_call(const greg_t *g) {
    switch (g[REG_RAX]) {
    case SYS_futex: {
        const int op=(int)g[REG_RSI]&~(FUTEX_PRIVATE_FLAG|FUTEX_CLOCK_REALTIME);
        return op==FUTEX_WAIT || op==FUTEX_WAIT_BITSET || op==FUTEX_LOCK_PI || op==FUTEX_WAIT_REQUEUE_PI;
    }
    case SYS_restart_syscall: case SYS_nanosleep: case SYS_clock_nanosleep: case SYS_poll: case SYS_ppoll:
    case SYS_select: case SYS_pselect6: case SYS_epoll_wait: case SYS_epoll_pwait: case SYS_rt_sigtimedwait:
    case SYS_rt_sigsuspend: case SYS_pause: case SYS_wait4: case SYS_waitid:
        return 1;
    default:
        return 0;
    }
}
/* Blocked in a wait. A signal is taken as any system call returns, and between instructions, so
 * on a syscall instruction rip is a wait the kernel restarts or a call about to run (the futex
 * wake of a condition signal under a runtime lock: still host code); rax tells which. A wait
 * interrupted for good returns EINTR; one not listed (a blocking read) shows up as the same
 * restart again and again. */
static int in_wait(GuestThread *t,const greg_t *g) {
    const unsigned char *r=(const unsigned char *)(uintptr_t)g[REG_RIP];
    if (r[-2]==0x0f && r[-1]==0x05 && g[REG_RAX]==-EINTR) return 1;
    if (r[0]!=0x0f || r[1]!=0x05) return 0;
    if (wait_call(g)) return 1;
    if ((uint64_t)g[REG_RIP]==t->raise_rip && (uint64_t)g[REG_RSP]==t->raise_rsp) return ++t->raise_still>=3;
    t->raise_rip=(uint64_t)g[REG_RIP]; t->raise_rsp=(uint64_t)g[REG_RSP]; t->raise_still=0;
    return 0;
}
/* Runs on the raised thread, whose GS still holds its guest TCB; the handler starts with a fresh FPU
 * state, so the PS4's comes first. Registers the handler changes are written back on return. The
 * raiser passes the thread record; a thread running host code answers without running the handler
 * (it may hold a lock the handler takes) and is signalled again. */
static void exception_signal_handler(int sig,siginfo_t *info,void *context) {
    (void)sig;
    greg_t *g=((ucontext_t *)context)->uc_mcontext.gregs;
    GuestThread *t=info->si_code==SI_QUEUE ? info->si_value.sival_ptr : NULL;
    if (t) {
        const uint32_t seq=__atomic_load_n(&t->raise_seq,__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&t->raise_done,__ATOMIC_RELAXED)==seq) return; /* answered already */
        const uint64_t rip=(uint64_t)g[REG_RIP];
        t->raise_ran=t->raise_force || !host_code(rip) || in_wait(t,g);
        __atomic_store_n(&t->raise_done,seq,__ATOMIC_RELEASE);
        if (!t->raise_ran) return;
    }
    ExceptionHandler handler=__atomic_load_n(&exception_handlers[30-1],__ATOMIC_ACQUIRE);
    if (!handler) return;
    int saved_errno=errno;
    runtime_orbis_fpu();
    GuestUcontext u={0};
    GuestMcontext *m=&u.mc;
    m->r8=(uint64_t)g[REG_R8]; m->r9=(uint64_t)g[REG_R9]; m->r10=(uint64_t)g[REG_R10]; m->r11=(uint64_t)g[REG_R11];
    m->r12=(uint64_t)g[REG_R12]; m->r13=(uint64_t)g[REG_R13]; m->r14=(uint64_t)g[REG_R14]; m->r15=(uint64_t)g[REG_R15];
    m->rdi=(uint64_t)g[REG_RDI]; m->rsi=(uint64_t)g[REG_RSI]; m->rbp=(uint64_t)g[REG_RBP]; m->rbx=(uint64_t)g[REG_RBX];
    m->rdx=(uint64_t)g[REG_RDX]; m->rax=(uint64_t)g[REG_RAX]; m->rcx=(uint64_t)g[REG_RCX];
    m->rip=(uint64_t)g[REG_RIP]; m->rflags=(uint64_t)g[REG_EFL];
    __asm__("mov %%fs,%0\n mov %%gs,%1" : "=r"(m->fs), "=r"(m->gs));
    const uint64_t reported=(uint64_t)(uintptr_t)&u;
    m->rsp=reported;
    __atomic_add_fetch(&exceptions_entered,1,__ATOMIC_RELAXED);
    handler(30,&u);
    g[REG_R8]=(greg_t)m->r8; g[REG_R9]=(greg_t)m->r9; g[REG_R10]=(greg_t)m->r10; g[REG_R11]=(greg_t)m->r11;
    g[REG_R12]=(greg_t)m->r12; g[REG_R13]=(greg_t)m->r13; g[REG_R14]=(greg_t)m->r14; g[REG_R15]=(greg_t)m->r15;
    g[REG_RDI]=(greg_t)m->rdi; g[REG_RSI]=(greg_t)m->rsi; g[REG_RBP]=(greg_t)m->rbp; g[REG_RBX]=(greg_t)m->rbx;
    g[REG_RDX]=(greg_t)m->rdx; g[REG_RAX]=(greg_t)m->rax; g[REG_RCX]=(greg_t)m->rcx;
    g[REG_RIP]=(greg_t)m->rip;
    if (m->rsp!=reported) g[REG_RSP]=(greg_t)m->rsp;
    __atomic_add_fetch(&exceptions_delivered,1,__ATOMIC_RELAXED);
    errno=saved_errno;
}
static void install_host_signal(void) {
    static int installed;
    if (__atomic_exchange_n(&installed,1,__ATOMIC_ACQ_REL)) return;
    struct sigaction sa={0};
    sa.sa_sigaction=exception_signal_handler;
    sa.sa_flags=SA_SIGINFO|SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(EXCEPTION_HOST_SIGNAL,&sa,NULL)) { perror("STOP: sigaction (guest exceptions)"); exit(21); }
}
#endif
static ABI int32_t raise_exception(GuestThread *t,int32_t signum) {
    if (signum!=30) return ERR(22);
    if (!live_thread(t)) return ERR(3);
    __atomic_add_fetch(&exceptions_raised,1,__ATOMIC_RELAXED);
#ifdef _WIN32
    if (t==current) { deliver_to_self(signum); return 0; }
    Proxy *x=malloc(sizeof(*x));
    if (!x) return ERR(12);
    HANDLE h;
    int proxy_it=0, still=0;
    DWORD64 last_rip=0, last_rsp=0;
    ULONG64 last_cycles=0;
    for (unsigned tries=0;;++tries) {
        /* Suspended under the lock its exit takes: the handle stays open and the thread alive. */
        pthread_mutex_lock(&lock);
        h=t->finished ? NULL : t->os_handle;
        DWORD suspended=h ? SuspendThread(h) : (DWORD)-1;
        pthread_mutex_unlock(&lock);
        if (suspended==(DWORD)-1) { free(x); return ERR(3); }
        *x=(Proxy){.t=t,.h=h,.signum=signum};
        x->c.ContextFlags=CONTEXT_FULL;
        if (!GetThreadContext(h,&x->c)) { ResumeThread(h); free(x); return ERR(3); }
        const unsigned char *rip=(const unsigned char *)(uintptr_t)x->c.Rip;
        if (rip[-2]==0x0f && rip[-1]==0x05) { /* stopped in a system call */
            if (wait_call(x->c.Rax) || tries>=RAISE_TRIES) { proxy_it=1; break; }
            /* Another call, or a wait not listed (hooked stub, socket): blocked once it stays there
             * three times without running in between. */
            ULONG64 cycles=0;
            QueryThreadCycleTime(h,&cycles);
            if (x->c.Rip==last_rip && x->c.Rsp==last_rsp && cycles==last_cycles) {
                if (++still==3) { proxy_it=1; break; }
            } else still=0;
            last_rip=x->c.Rip; last_rsp=x->c.Rsp; last_cycles=cycles;
            ResumeThread(h);
            Sleep(1);
            continue;
        }
        leave_resume(&x->c);
        if (!host_code(x->c.Rip)) break;
        /* Inside the runtime, a host library or a handler on its way out: it may hold a lock the
         * handler needs (every event flag shares one, and the IL2CPP handler waits on a flag), so
         * it runs on until it is back in guest code or blocked. */
        if (tries==RAISE_TRIES) {
            printf("Runtime: thread '%s' stayed in host code at %p for a guest exception; handled from another thread\n",
                   t->name,(void *)(uintptr_t)x->c.Rip);
            proxy_it=1;
            break;
        }
        ResumeThread(h);
        if (tries<64) SwitchToThread(); else Sleep(1);
    }
    if (proxy_it) {
        pthread_t proxy; pthread_attr_t attr;
        pthread_attr_init(&attr); pthread_attr_setstacksize(&attr,DEFAULT_STACK);
        int e=pthread_create(&proxy,&attr,proxy_deliver,x);
        pthread_attr_destroy(&attr);
        if (e) { ResumeThread(h); free(x); return ERR(11); }
        pthread_detach(proxy);
        __atomic_add_fetch(&raised_proxy,1,__ATOMIC_RELAXED);
        return 0;
    }
    /* Running code: it resumes at redzone_deliver, below its red zone. */
    leave_resume(&x->c);
    uint64_t *slot=(uint64_t *)(uintptr_t)(x->c.Rsp-RESUME_GAP);
    slot[0]=x->c.Rip; slot[1]=x->c.Rsp; slot[-1]=(uint64_t)signum;
    x->c.Rip=(DWORD64)(uintptr_t)redzone_deliver; x->c.Rsp=(DWORD64)(uintptr_t)(slot-1);
    x->c.ContextFlags=CONTEXT_CONTROL;
    const BOOL moved=SetThreadContext(h,&x->c);
    free(x);
    if (moved) __atomic_add_fetch(&raised_running,1,__ATOMIC_RELAXED);
    ResumeThread(h);
    return moved ? 0 : ERR(3);
#else
    install_host_signal();
    /* Raised on the calling thread: the signal is taken before pthread_sigqueue returns, so the lock
     * (which the handler's guest code may need) is not held, and the handler always runs. Another
     * thread is signalled under the lock its exit takes, so its pthread_t stays valid, and again
     * while it answers from host code: it runs on until it is back in guest code or blocked. One
     * raise at a time waits for the answers. */
    static pthread_mutex_t raise_lock=PTHREAD_MUTEX_INITIALIZER;
    const union sigval value={.sival_ptr=t};
    if (t==current) {
        __atomic_store_n(&t->raise_force,1,__ATOMIC_RELAXED);
        __atomic_add_fetch(&t->raise_seq,1,__ATOMIC_RELEASE);
        if (pthread_sigqueue(t->host,EXCEPTION_HOST_SIGNAL,value)) return ERR(3);
        __atomic_add_fetch(&raised_running,1,__ATOMIC_RELAXED);
        return 0;
    }
    pthread_mutex_lock(&raise_lock);
    int32_t r=0;
    for (unsigned tries=0;;++tries) {
        pthread_mutex_lock(&lock);
        uint32_t seq=0;
        int e=ESRCH;
        if (!t->finished) {
            __atomic_store_n(&t->raise_force,tries>=RAISE_TRIES,__ATOMIC_RELAXED);
            seq=__atomic_add_fetch(&t->raise_seq,1,__ATOMIC_RELEASE);
            e=pthread_sigqueue(t->host,EXCEPTION_HOST_SIGNAL,value);
        }
        pthread_mutex_unlock(&lock);
        if (e) { r=ERR(3); break; }
        if (tries==RAISE_TRIES)
            printf("Runtime: thread '%s' stayed in host code for a guest exception; handled there\n",t->name);
        /* Taken at once unless the thread is gone or has the signal blocked: after about a second
         * it stays queued, to run the handler whenever it is taken (as before answers were awaited). */
        for (unsigned w=0;__atomic_load_n(&t->raise_done,__ATOMIC_ACQUIRE)!=seq;++w) {
            if (__atomic_load_n(&t->finished,__ATOMIC_ACQUIRE)) { r=ERR(3); break; }
            if (w==1064) { __atomic_store_n(&t->raise_force,1,__ATOMIC_RELAXED); break; }
            if (w<64) sched_yield(); else usleep(1000);
        }
        if (r || __atomic_load_n(&t->raise_done,__ATOMIC_ACQUIRE)!=seq) break;
        if (t->raise_ran) break;
        if (tries<64) sched_yield(); else usleep(1000);
    }
    pthread_mutex_unlock(&raise_lock);
    if (!r) __atomic_add_fetch(&raised_running,1,__ATOMIC_RELAXED);
    return r;
#endif
}

/* POSIX (libScePosix) variants return positive errno values. */
static int32_t posix(int32_t r) { return r ? (int32_t)((uint32_t)r&0xffff) : 0; }
static ABI int32_t posix_attr_init(ThreadAttr **a) { return posix(attr_init(a)); }
static ABI int32_t posix_attr_destroy(ThreadAttr **a) { return posix(attr_destroy(a)); }
static ABI int32_t posix_attr_set_detach(ThreadAttr **a,int s) { return posix(attr_set_detach(a,s)); }
static ABI int32_t posix_attr_set_stack(ThreadAttr **a,uint64_t s) { return posix(attr_set_stack(a,s)); }
static ABI int32_t posix_attr_set_param(ThreadAttr **a,const int *p) { return posix(attr_set_param(a,p)); }
static ABI int32_t posix_create(GuestThread **t,ThreadAttr **a,GuestEntry e,void *arg) { return posix(create(t,a,e,arg,"posix")); }
static ABI int32_t posix_create_name(GuestThread **t,ThreadAttr **a,GuestEntry e,void *arg,const char *name) { return posix(create(t,a,e,arg,name)); }
static ABI int32_t posix_join(GuestThread *t,void **r) { return posix(thread_join(t,r)); }
static ABI int32_t posix_detach(GuestThread *t) { return t ? posix(thread_detach(t)) : 22; }
static ABI int32_t posix_get_sched(GuestThread *t,int *p,int *s) { return posix(thread_get_sched(t,p,s)); }
static ABI int32_t posix_set_sched(GuestThread *t,int p,const int *s) { return posix(thread_set_sched(t,p,s)); }

uintptr_t runtime_thread_resolve(const char *name) {
    static const struct { const char *nid; void *fn; } table[]={
        {"aI+OeCz8xrQ#libkernel",thread_self}, {"EotR8a3ASf4#libkernel",thread_self},
        {"9BcDykPmo1I#libkernel",guest_error},
        {"nsYoNRywwNg#libkernel",attr_init}, {"62KCwEMmzcM#libkernel",attr_destroy},
        {"x1X76arYMxU#libkernel",attr_get}, {"8+s5BzZjxSg#libkernel",attr_affinity},
        {"UTXzJbWhhTE#libkernel",attr_set_stack}, {"-Wreprtu0Qs#libkernel",attr_set_detach},
        {"4+h9EzwKF4I#libkernel",attr_set_policy}, {"DzES9hQF4f4#libkernel",attr_set_param},
        {"3qxgM4ezETA#libkernel",attr_set_affinity}, {"eXbUSpEaTsA#libkernel",attr_set_inherit},
        {"JaRMy+QcpeU#libkernel",attr_get_detach}, {"-fA+7ZlGDQs#libkernel",attr_get_stack},
        {"FXPWHNk8Of0#libkernel",attr_get_param}, {"El+cQ20DynU#libkernel",attr_set_guard},
        {"6UgtwV+0zb4#libkernel",thread_create}, {"onNY9Byn-W8#libkernel",thread_join},
        {"4qGrR6eoP9Y#libkernel",thread_detach}, {"3kg7rT0NQIs#libkernel",thread_exit},
        {"T72hz6ffq08#libkernel",thread_yield}, {"1tKyG7RlMJo#libkernel",thread_get_prio},
        {"W0Hpm2X0uPE#libkernel",thread_set_prio}, {"bt3CTBKmGyI#libkernel",thread_set_affinity}, {"rcrVFJsQWRY#libkernel",thread_get_affinity},
        {"GBUY7ywdULE#libkernel",thread_rename}, {"3PtV6p3QNX4#libkernel",thread_equal},
        {"wtkt-teR1so#libkernel",posix_attr_init}, {"zHchY8ft5pk#libkernel",posix_attr_destroy},
        {"E+tyo3lp5Lw#libkernel",posix_attr_set_detach}, {"2Q0z6rnBrTE#libkernel",posix_attr_set_stack},
        {"euKRgm0Vn2M#libkernel",posix_attr_set_param}, {"OxhIB8LB-PQ#libkernel",posix_create},
        {"Jmi+9w9u0E4#libkernel",posix_create_name}, {"h9CcP3J0oVM#libkernel",posix_join},
        {"FJrT5LuUBAU#libkernel",thread_exit},
        {"P41kTWUS3EI#libkernel",thread_get_sched}, {"oIRFTjoILbg#libkernel",thread_set_sched},
        {"How7B8Oet6k#libkernel",thread_get_name}, {"EI-5-jlq2dE#libkernel",thread_get_id},
        {"Bvn74vj6oLo#libkernel",attr_set_stack_area}, {"-quPa4SEJUw#libkernel",attr_get_stack_area},
        {"Ru36fiTtJzA#libkernel",attr_get_stack_address},
        {"FIs3-UQT9sg#libkernel",posix_get_sched}, {"Xs9hdiD7sAA#libkernel",posix_set_sched},
        {"+U1R4WtXvoc#libkernel",posix_detach}, {"7Xl257M4VNI#libkernel",thread_equal},
        {"CBNtXOoef-E#libkernel",priority_max}, {"m0iS6jNsXds#libkernel",priority_min},
        {"lZzFeSxPl08#libkernel",set_cancel_state}, {"0D4-FVvEikw#libkernel",posix_cancel},
        {"1xvtUVx1-Sg#libkernel",cleanup_push}, {"iWsFlYMf3Kw#libkernel",cleanup_pop},
        {"Tz4RNUCBbGI#libkernel",rtld_atexit_increment}, {"8OnWXlgQlvo#libkernel",rtld_atexit_decrement},
        {"WkwEd3N7w0Y#libkernel",install_exception_handler}, {"WkwEd3N7w0Y#libkernel_unity",install_exception_handler},
        {"Qhv5ARAoOEc#libkernel",remove_exception_handler}, {"Qhv5ARAoOEc#libkernel_unity",remove_exception_handler},
        {"il03nluKfMk#libkernel_unity",raise_exception},
    };
    for (size_t i=0;i<sizeof(table)/sizeof(*table);++i)
        if (!strcmp(name,table[i].nid)) return (uintptr_t)table[i].fn;
    return 0;
}
/* ---- CPU time per host thread (which threads keep 4-thread CPUs at 100%) ---- */
static uint64_t guest_yields, guest_zero_sleeps;
void runtime_thread_count_yield(int zero_sleep) {
    __atomic_fetch_add(zero_sleep ? &guest_zero_sleeps : &guest_yields,1,__ATOMIC_RELAXED);
}
static struct timespec session_start;
__attribute__((constructor)) static void mark_session_start(void) { clock_gettime(CLOCK_MONOTONIC,&session_start); }
static double seconds_since(const struct timespec *t) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
    return (double)(now.tv_sec-t->tv_sec)+(double)(now.tv_nsec-t->tv_nsec)/1e9;
}
typedef struct { char name[32]; double ms; } ThreadCpu;
enum { CPU_REPORT_MAX = 1024, CPU_REPORT_SHOWN = 15 };
static int by_cpu_desc(const void *a,const void *b) {
    double x=((const ThreadCpu *)a)->ms, y=((const ThreadCpu *)b)->ms;
    return (x<y)-(x>y);
}
/* Live threads of this process with their CPU time; threads that already exited are not listed. */
static size_t live_thread_cpu(ThreadCpu *out,size_t max) {
    size_t n=0;
#ifdef _WIN32
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if (snap==INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 entry={.dwSize=sizeof(entry)};
    DWORD pid=GetCurrentProcessId();
    for (BOOL ok=Thread32First(snap,&entry);ok && n<max;ok=Thread32Next(snap,&entry)) {
        if (entry.th32OwnerProcessID!=pid) continue;
        HANDLE h=OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ThreadID);
        if (!h) continue;
        FILETIME c,e,k,u;
        if (GetThreadTimes(h,&c,&e,&k,&u)) {
            ThreadCpu *t=&out[n++];
            uint64_t ticks=(((uint64_t)k.dwHighDateTime<<32)|k.dwLowDateTime)+(((uint64_t)u.dwHighDateTime<<32)|u.dwLowDateTime);
            t->ms=(double)ticks/1e4;
            PWSTR wide=NULL;
            t->name[0]=0;
            if (SUCCEEDED(GetThreadDescription(h,&wide)) && wide) {
                WideCharToMultiByte(CP_UTF8,0,wide,-1,t->name,sizeof(t->name),NULL,NULL);
                LocalFree(wide);
            }
            if (!t->name[0]) snprintf(t->name,sizeof(t->name),"tid %lu",entry.th32ThreadID);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
#else
    DIR *dir=opendir("/proc/self/task");
    if (!dir) return 0;
    long hz=sysconf(_SC_CLK_TCK);
    for (struct dirent *d;(d=readdir(dir)) && n<max;) {
        if (d->d_name[0]=='.') continue;
        char path[sizeof(d->d_name)+sizeof("/proc/self/task//stat")], line[1024];
        snprintf(path,sizeof(path),"/proc/self/task/%s/stat",d->d_name);
        FILE *f=fopen(path,"r");
        if (!f) continue;
        size_t len=fread(line,1,sizeof(line)-1,f); fclose(f);
        line[len]=0;
        char *open=strchr(line,'('), *close=strrchr(line,')');
        unsigned long utime, stime;
        /* After "(comm)": state, then fields 4..13, then utime and stime (proc(5)). */
        if (!open || !close || sscanf(close+2,"%*c %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %lu %lu",&utime,&stime)!=2) continue;
        ThreadCpu *t=&out[n++];
        t->ms=(double)(utime+stime)*1000.0/(double)(hz>0 ? hz : 100);
        snprintf(t->name,sizeof(t->name),"%.*s",(int)(close-open-1),open+1);
    }
    closedir(dir);
#endif
    return n;
}
static void cpu_report(void) {
    static ThreadCpu list[CPU_REPORT_MAX];
    size_t n=live_thread_cpu(list,CPU_REPORT_MAX);
    double wall=seconds_since(&session_start), total=0;
    for (size_t i=0;i<n;++i) total+=list[i].ms;
    qsort(list,n,sizeof(*list),by_cpu_desc);
    /* The affinity mask, so `start /affinity F` or taskset emulating a 4-thread CPU shows 4. */
    long hw=0;
#ifdef _WIN32
    DWORD_PTR process_mask=0, system_mask=0;
    if (GetProcessAffinityMask(GetCurrentProcess(),&process_mask,&system_mask)) hw=__builtin_popcountll(process_mask);
#else
    cpu_set_t set; CPU_ZERO(&set);
    if (!sched_getaffinity(0,sizeof(set),&set)) hw=CPU_COUNT(&set);
#endif
    printf("Runtime: CPU time by thread: session %.0f s, %zu live threads, %.0f s CPU (%.0f%% of %ld hardware threads)\n",
           wall,n,total/1000.0,wall>0 && hw>0 ? total/10.0/wall/(double)hw : 0.0,hw);
    for (size_t i=0;i<n && i<CPU_REPORT_SHOWN;++i)
        printf("Runtime:   %-24s %9.0f ms (%5.1f%% of one thread)\n",list[i].name,list[i].ms,wall>0 ? list[i].ms/10.0/wall : 0.0);
    printf("Runtime: yields guest=%llu, zero sleeps=%llu\n",
           (unsigned long long)__atomic_load_n(&guest_yields,__ATOMIC_RELAXED),
           (unsigned long long)__atomic_load_n(&guest_zero_sleeps,__ATOMIC_RELAXED));
}
void runtime_thread_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: guest threads created=%zu, exited=%zu, joined=%zu, thread-exit destructors pending=%lld\n",
           created,exited,joined_count,(long long)__atomic_load_n(&rtld_atexit_pending,__ATOMIC_RELAXED));
    pthread_mutex_unlock(&lock);
    printf("Runtime: exceptions raised=%llu (running %llu, proxy %llu), handlers entered=%llu, returned=%llu\n",
           (unsigned long long)__atomic_load_n(&exceptions_raised,__ATOMIC_RELAXED),
           (unsigned long long)__atomic_load_n(&raised_running,__ATOMIC_RELAXED),
           (unsigned long long)__atomic_load_n(&raised_proxy,__ATOMIC_RELAXED),
           (unsigned long long)__atomic_load_n(&exceptions_entered,__ATOMIC_RELAXED),
           (unsigned long long)__atomic_load_n(&exceptions_delivered,__ATOMIC_RELAXED));
    cpu_report();
}

