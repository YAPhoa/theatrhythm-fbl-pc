/* Thread scheduling/name/stack calls (src/runtime_thread.c) against the contracts shadPS4 follows. */
#include "../src/runtime_thread.c"
#include <assert.h>

void *runtime_low_alloc(size_t size) { return calloc(1, size); }
void runtime_low_free(void *pointer) { free(pointer); }
/* Linux guest stacks: none low here, so pthreads places them. */
void *runtime_low_map(size_t size, int prot) { (void)size; (void)prot; return NULL; }
void runtime_thread_keys_cleanup(void) {}
int runtime_verbose(void) { return 0; }

static int32_t own_tid, own_cancel;
static char own_name[32];
static void *ABI inspector(void *arg) {
    (void)arg;
    own_tid = thread_get_id();
    int old = -1;
    own_cancel = set_cancel_state(1, &old) == 0 && old == 0 && set_cancel_state(0, &old) == 0 && old == 1;
    assert(set_cancel_state(2, &old) == 22);
    thread_get_name(runtime_thread_current(), own_name);
    return NULL;
}

/* sceKernelRaiseException: the handler runs on the target thread with its context. */
static volatile int spin_ready;
static GuestThread *volatile handled_on;
static volatile uint64_t handled_sp;
/* Guest code: a copy in low executable memory outside any loaded module, as the game's image is
 * (a thread running host code is let finish before a raise reaches it). Each loop takes its stop
 * and ready flags through rdi. */
typedef struct { volatile int stop, ready; } SpinFlags;
typedef void *(ABI *GuestEntry)(void *);
static GuestEntry guest_copy(const void *start, const void *end) {
    size_t size = (size_t)((const char *)end - (const char *)start);
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(p);
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(p != MAP_FAILED);
#endif
    memcpy(p, start, size);
    return (GuestEntry)p;
}
/* Sets ready, spins until stop. */
void spinner_code(void), spinner_end(void);
__asm__(".text\n.globl spinner_code\nspinner_code:\n"
        " movl $1,4(%rdi)\n1: pause\n cmpl $0,(%rdi)\n je 1b\n xor %eax,%eax\n ret\n"
        ".globl spinner_end\nspinner_end:\n");
static pthread_mutex_t block_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t block_wake = PTHREAD_COND_INITIALIZER;
static int block_release;
static void *ABI blocker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&block_lock);
    spin_ready = 1;
    while (!block_release) pthread_cond_wait(&block_wake, &block_lock);
    pthread_mutex_unlock(&block_lock);
    return NULL;
}
/* A SysV leaf loop with 16 words of pattern in its red zone (the 128 bytes below rsp) and in r8-r11
 * and xmm0 (caller-saved: a handler may change them), checked until stop is set: returns
 * NULL while they hold, 1 once one changed. */
void redzone_code(void), redzone_end(void);
__asm__(".text\n.globl redzone_code\nredzone_code:\n"
        " movabs $0x5a5a5a5a00000000,%rax\n xor %ecx,%ecx\n"
        "1: lea (%rax,%rcx),%rdx\n mov %rdx,-128(%rsp,%rcx,8)\n inc %ecx\n cmp $16,%ecx\n jb 1b\n"
        " lea 16(%rax),%r8\n lea 17(%rax),%r9\n lea 18(%rax),%r10\n lea 19(%rax),%r11\n movq %rax,%xmm0\n"
        " movl $1,4(%rdi)\n"
        "2: xor %ecx,%ecx\n"
        "3: lea (%rax,%rcx),%rdx\n cmp %rdx,-128(%rsp,%rcx,8)\n jne 4f\n inc %ecx\n cmp $16,%ecx\n jb 3b\n"
        " lea 16(%rax),%rdx\n cmp %rdx,%r8\n jne 4f\n lea 17(%rax),%rdx\n cmp %rdx,%r9\n jne 4f\n"
        " lea 18(%rax),%rdx\n cmp %rdx,%r10\n jne 4f\n lea 19(%rax),%rdx\n cmp %rdx,%r11\n jne 4f\n"
        " movq %xmm0,%rdx\n cmp %rax,%rdx\n jne 4f\n"
        " cmpl $0,(%rdi)\n je 2b\n xor %eax,%eax\n ret\n"
        "4: mov $1,%eax\n ret\n"
        ".globl redzone_end\nredzone_end:\n");
static void ABI on_exception(int32_t signum,GuestUcontext *u) {
    if (signum==30) { handled_sp=*(uint64_t *)((char *)u+0xf8); handled_on=runtime_thread_current(); }
}
/* Host code (this exe, with a system call) between short guest calls; host_inside marks the host
 * part. */
static volatile int host_inside, host_stop, host_ready, handled_inside_host;
void guest_pause_code(void), guest_pause_end(void);
__asm__(".text\n.globl guest_pause_code\nguest_pause_code:\n"
        " mov $200,%ecx\n1: pause\n dec %ecx\n jnz 1b\n xor %eax,%eax\n ret\n"
        ".globl guest_pause_end\nguest_pause_end:\n");
static void quick_syscall(void) {
#ifdef _WIN32
    static HANDLE event;
    if (!event) event = CreateEventA(NULL, TRUE, FALSE, NULL);
    SetEvent(event);
#else
    syscall(SYS_getppid);
#endif
}
static void *ABI host_worker(void *arg) {
    GuestEntry guest_pause = (GuestEntry)arg;
    host_ready = 1;
    while (!host_stop) {
        host_inside = 1;
        for (volatile int i = 0; i < 1000; ++i) {}
        quick_syscall(); /* returning from it, the thread is not blocked: still host code */
        for (volatile int i = 0; i < 1000; ++i) {}
        host_inside = 0;
        guest_pause(NULL);
    }
    return NULL;
}
static void ABI host_inside_check(int32_t signum,GuestUcontext *u) {
    (void)u;
    if (signum==30 && host_inside) handled_inside_host=1;
}
/* A handler that uses the caller-saved registers, as compiled guest code may. */
static void ABI clobbering_exception(int32_t signum,GuestUcontext *u) {
    __asm__ volatile("mov $-1,%%r8\n mov $-1,%%r9\n mov $-1,%%r10\n mov $-1,%%r11\n pcmpeqd %%xmm0,%%xmm0"
                     ::: "r8","r9","r10","r11","xmm0","cc");
    on_exception(signum,u);
}

/* pthread_cleanup_push/pop: pop(1) runs the innermost handler, pthread_exit the rest. */
static char cleanup_log[8];
static void ABI note_cleanup(void *arg) { strncat(cleanup_log,(const char *)arg,1); }
static void *ABI cleaner(void *arg) {
    (void)arg;
    void *outer[8], *inner[8], *popped[8];
    cleanup_push(note_cleanup,"a",outer);
    cleanup_push(note_cleanup,"b",inner);
    cleanup_push(note_cleanup,"x",popped);
    cleanup_pop(0);
    cleanup_pop(1);
    cleanup_push(note_cleanup,"c",inner);
    thread_exit(NULL);
}

int main(void) {
    /* A thread that has finished. */
    GuestThread *t = NULL;
    assert(thread_create(&t, NULL, inspector, NULL, "finished") == 0);
    assert(thread_join(t, NULL) == 0);

    /* Scheduling parameters of a live thread; a finished one is ESRCH. */
    GuestThread *self = runtime_thread_current();
    int policy = 0, prio = 0;
    assert(thread_get_sched(self, &policy, &prio) == 0 && policy == 1 && prio == DEFAULT_PRIO);
    int param = 300;
    assert(thread_set_sched(self, 3, &param) == 0);
    assert(posix_get_sched(self, &policy, &prio) == 0 && policy == 3 && prio == 300);
    assert(thread_get_sched(self, NULL, &prio) == ERR(22) && posix_get_sched(self, NULL, &prio) == 22);
    assert(thread_get_sched(t, &policy, &prio) == ERR(3) && posix_set_sched(t, 1, &param) == 3);
    assert(priority_max(1) == 767 && priority_min(3) == 256 && priority_max(2) == 22);

    /* Names, ids, cancel state: on the thread itself. */
    assert(thread_create(&t, NULL, inspector, NULL, "inspector") == 0);
    assert(thread_join(t, NULL) == 0);
    assert(!strcmp(own_name, "inspector") && own_tid > 0 && own_tid != thread_get_id() && own_cancel);
    char name[32];
    assert(thread_get_name(self, name) == 0 && thread_get_name(self, NULL) == ERR(22));
    assert(thread_get_name((GuestThread *)name, name) == ERR(3));
    assert(posix_detach(NULL) == 22);

    /* Stack area of an attribute. */
    ThreadAttr *attr = NULL;
    void *address = NULL;
    uint64_t size = 0;
    static unsigned char area[65536];
    assert(attr_init(&attr) == 0);
    assert(attr_set_stack_area(&attr, area, 4096) == ERR(22) && attr_set_stack_area(&attr, NULL, 65536) == ERR(22));
    assert(attr_set_stack_area(&attr, area, sizeof(area)) == 0);
    assert(attr_get_stack_area(&attr, &address, &size) == 0 && address == area && size == sizeof(area));
    assert(attr_get_stack_address(&attr, &address) == 0 && address == area);
    assert(attr_destroy(&attr) == 0);
    /* scePthreadAttrGet reports the stack the thread runs on: it holds this frame's locals. */
    assert(attr_init(&attr) == 0 && attr_get(self, &attr) == 0);
    assert(attr_get_stack_area(&attr, &address, &size) == 0 && address && size);
    char local = 0;
    assert((char *)address <= &local && &local < (char *)address + size);
    assert(attr_destroy(&attr) == 0);

    /* Exception handlers: allowed signals, one handler each, delivery to a running thread. */
    assert(install_exception_handler(30, on_exception) == 0 && install_exception_handler(30, on_exception) == ERR(35));
    assert(install_exception_handler(2, on_exception) == ERR(22) && remove_exception_handler(2) == ERR(22));
    assert(raise_exception(self, 11) == ERR(22) && raise_exception((GuestThread *)name, 30) == ERR(3));
    SpinFlags spin = {0, 0};
    assert(thread_create(&t, NULL, guest_copy(spinner_code, spinner_end), &spin, "spinner") == 0);
    while (!spin.ready) {}
    assert(raise_exception(t, 30) == 0);
    for (int i = 0; i < 2000 && !handled_on; ++i) usleep(1000);
    spin.stop = 1;
    assert(thread_join(t, NULL) == 0);
    assert(handled_on == t && handled_sp && exceptions_delivered == 1);
    /* A thread blocked in a wait: the handler runs as that thread while it stays suspended. */
    spin_ready = 0; handled_on = NULL; handled_sp = 0;
    assert(thread_create(&t, NULL, blocker, NULL, "blocker") == 0);
    while (!spin_ready) {}
    pthread_mutex_lock(&block_lock); pthread_mutex_unlock(&block_lock); /* now in pthread_cond_wait */
    usleep(50000);
    assert(raise_exception(t, 30) == 0);
    for (int i = 0; i < 2000 && !handled_on; ++i) usleep(1000);
    assert(handled_on == t && handled_sp && exceptions_delivered == 2);
    pthread_mutex_lock(&block_lock); block_release = 1; pthread_cond_signal(&block_wake); pthread_mutex_unlock(&block_lock);
    assert(thread_join(t, NULL) == 0);
    /* A detached thread too (winpthreads closes its handle; the record keeps its own). */
    spin_ready = 0; handled_on = NULL; block_release = 0;
    ThreadAttr *detached = NULL;
    assert(attr_init(&detached) == 0 && attr_set_detach(&detached, 1) == 0);
    assert(thread_create(&t, &detached, blocker, NULL, "detached") == 0);
    while (!spin_ready) {}
    pthread_mutex_lock(&block_lock); pthread_mutex_unlock(&block_lock);
    usleep(50000);
    assert(raise_exception(t, 30) == 0);
    for (int i = 0; i < 2000 && !handled_on; ++i) usleep(1000);
    assert(handled_on == t && exceptions_delivered == 3);
    pthread_mutex_lock(&block_lock); block_release = 1; pthread_cond_signal(&block_wake); pthread_mutex_unlock(&block_lock);
    for (int i = 0; i < 2000 && !__atomic_load_n(&t->finished, __ATOMIC_ACQUIRE); ++i) usleep(1000);
    assert(raise_exception(t, 30) == ERR(3) && attr_destroy(&detached) == 0);
    assert(remove_exception_handler(30) == 0 && install_exception_handler(30, clobbering_exception) == 0);
    /* Raised on the calling thread: handled before the raise returns. */
    handled_on = NULL;
    assert(raise_exception(self, 30) == 0 && handled_on == self && exceptions_delivered == 4);
    /* A thread interrupted in a leaf function keeps its red zone and registers (the handler runs below). */
    SpinFlags redzone = {0, 0};
    assert(thread_create(&t, NULL, guest_copy(redzone_code, redzone_end), &redzone, "redzone") == 0);
    while (!redzone.ready) {}
    const uint64_t running_before = raised_running;
    for (int i = 0; i < 50; ++i) {
        const uint64_t delivered = exceptions_delivered;
        assert(raise_exception(t, 30) == 0);
        for (int w = 0; w < 2000 && exceptions_delivered == delivered; ++w) usleep(100);
        assert(exceptions_delivered == delivered + 1);
    }
    /* Back to back, so raises land while the last delivery is still running (nested). */
    const uint64_t burst = exceptions_delivered;
    for (int i = 0; i < 50; ++i) assert(raise_exception(t, 30) == 0);
    for (int w = 0; w < 20000 && exceptions_delivered < burst + 50; ++w) usleep(100);
    assert(exceptions_delivered == burst + 50);
    redzone.stop = 1;
    void *clobbered = (void *)1;
    assert(thread_join(t, &clobbered) == 0 && clobbered == NULL && raised_running >= running_before + 50);
    /* A thread that keeps entering host code gets the handler only back in guest code: in host
     * code it may hold a lock the handler takes (the event flags' one). */
    assert(remove_exception_handler(30) == 0 && install_exception_handler(30, host_inside_check) == 0);
    assert(thread_create(&t, NULL, host_worker, guest_copy(guest_pause_code, guest_pause_end), "host worker") == 0);
    while (!host_ready) {}
    const uint64_t worker_before = raised_running;
    for (int i = 0; i < 200; ++i) {
        const uint64_t delivered = exceptions_delivered;
        assert(raise_exception(t, 30) == 0);
        for (int w = 0; w < 20000 && exceptions_delivered == delivered; ++w) usleep(100);
        assert(exceptions_delivered == delivered + 1);
    }
    host_stop = 1;
    assert(thread_join(t, NULL) == 0 && !handled_inside_host && raised_running == worker_before + 200);

    assert(thread_create(&t, NULL, cleaner, NULL, "cleaner") == 0 && thread_join(t, NULL) == 0);
    assert(!strcmp(cleanup_log, "bca"));
    cleanup_pop(1); /* nothing pushed on this thread: no effect */
    /* pthread_cancel: a finished thread is ESRCH; with cancellation disabled it stays pending. */
    assert(posix_cancel(t) == 3 && posix_cancel((GuestThread *)name) == 3);
    int old_state = 0;
    assert(set_cancel_state(1, &old_state) == 0 && posix_cancel(self) == 0 && self->cancel_pending);
    self->cancel_pending = 0;
    assert(set_cancel_state(0, &old_state) == 0 && old_state == 1);
    rtld_atexit_increment(); rtld_atexit_decrement();
    assert(rtld_atexit_pending == 0);
    puts("thread calls test: ok");
    return 0;
}
