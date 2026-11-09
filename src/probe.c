/* Minimal x86-64 native loader experiment. Not a PS4 emulator or game port. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <SDL3/SDL.h>
#include "runtime.h"
#include "cpu_check.h"
#include "isa_rewrite.h"
#include "gpu/theatrhythm_gpu.h"
#if !defined(__x86_64__) || !defined(__GNUC__)
#error This prototype requires x86-64 GCC or Clang (including MinGW).
#endif
#include "launch.h"
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#else
#include <pthread.h>
#include <dirent.h>
#include <sys/mman.h>
#include <malloc.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#endif

typedef struct { uint64_t address, size, flags; } Segment;
typedef struct { uint64_t target, kind, value, addend; } Reloc;
static char (*names)[128];
static uint64_t import_count;
static unsigned char *image;
static size_t page_size;
typedef RuntimeModule LinkedModule;
static LinkedModule modules[32];
static uint64_t module_count;
static RuntimeModule main_module; /* the eboot: unwind fields only */
static int entered_game;
static int gpu_enabled;
static unsigned cpu_missing_sets; /* CPU_SET_* the host lacks; the guest code needs them rewritten */
static IsaTable *isa_table; /* rewritten sites (--isa-rewrite); the fault handlers and the report read it */
static void loader_report_hook(void) {
    if (isa_table) isa_rewrite_report(isa_table, stdout);
    if (gpu_enabled) printf("GPU: copy waits yielded %llu times\n", theatrhythm_gpu_copy_yields());
}
int vulkan_smoke(void);

static void fail(const char *message) { fprintf(stderr, "ERROR: %s\n", message); exit(1); }
static uint64_t read64(FILE *f) {
    unsigned char b[8];
    if (fread(b, 1, 8, f) != 8) fail("truncated boot file");
    uint64_t n = 0;
    for (int i = 7; i >= 0; --i) n = (n << 8) | b[i];
    return n;
}
static size_t round_page(size_t size) { return (size + page_size - 1) & ~(page_size - 1); }
static void *allocate(size_t size) {
    void *low=runtime_low_map(size,PROT_READ|PROT_WRITE);
    if (low) return low;
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) fail("VirtualAlloc failed");
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fail("mmap failed");
#endif
    return p;
}
static void protect(void *p, size_t size, unsigned flags) {
#ifdef _WIN32
    DWORD old, mode = PAGE_NOACCESS;
    if (flags & 1) mode = (flags & 2) ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    else if (flags & 2) mode = PAGE_READWRITE;
    else if (flags & 4) mode = PAGE_READONLY;
    if (!VirtualProtect(p, size, mode, &old)) fail("VirtualProtect failed");
    FlushInstructionCache(GetCurrentProcess(), p, size);
#else
    int mode = ((flags & 4) ? PROT_READ : 0) | ((flags & 2) ? PROT_WRITE : 0) | ((flags & 1) ? PROT_EXEC : 0);
    if (mprotect(p, size, mode)) fail("mprotect failed");
#endif
}
static ABI __attribute__((noreturn)) void unresolved(uint32_t id, uintptr_t argument) {
    if (id >= import_count) fail("bad import trap index");
    printf("STOP: first unsupported PS4 import: %s (index %u)\n", names[id], id);
    printf("API: %s\n", runtime_import_name(names[id]));
    printf("Caller return offset: 0x%" PRIxPTR "; first argument: 0x%" PRIxPTR "\n",
           (uintptr_t)__builtin_return_address(0) - (uintptr_t)image, argument);
    uintptr_t caller=(uintptr_t)__builtin_return_address(0)-(uintptr_t)image;
    for (uint64_t m=0;m<module_count;++m)
        if (caller>=modules[m].base && caller-modules[m].base<modules[m].size)
            printf("Caller in linked module %" PRIu64 " (%s): +0x%" PRIxPTR "\n",m,modules[m].name[0] ? modules[m].name : "system module",caller-modules[m].base);
    runtime_report();
    puts(entered_game ? "Original guest entry instructions executed; game initialization is incomplete." :
                        "Native libc initialization is incomplete; game entry has not run.");
    fflush(NULL);
    _exit(20); /* no destructors: GPU, audio and guest threads are still running */
}
#ifndef _WIN32
/* enter_on_stack(entry, arg0, arg1, stack_top): call entry(arg0,arg1) on a new stack, with rbp 0
 * (the end of the frame chain, as runtime_enter_guest). */
void enter_on_stack(void *entry, void *arg0, void *arg1, void *top);
__asm__(".text\n.globl enter_on_stack\nenter_on_stack:\n"
        " push %rbp\n push %rbx\n mov %rsp,%rbx\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n xor %ebp,%ebp\n call *%rax\n"
        " mov %rbx,%rsp\n pop %rbx\n pop %rbp\n ret\n");
#endif
static ABI void guest_exit(void) { puts("Runtime: process finalizer callback reached"); }
#ifdef _WIN32
static void describe_address(char *out, size_t size, uintptr_t address) {
    HMODULE module=NULL;
    char name[MAX_PATH]="?";
    int site=isa_table ? isa_rewrite_site_of_stub(isa_table,address) : -1;
    if (site>=0 || (image && address-(uintptr_t)image<0x20000000)) {
        int n=snprintf(out,size,"guest offset 0x%" PRIxPTR,address-(uintptr_t)image);
        if (site>=0 && n>=0 && (size_t)n<size)
            snprintf(out+n,size-n," While emulating site 0x%" PRIx64 " (%s)",isa_table->sites[site].offset,isa_kind_name(isa_table->sites[site].kind));
        return;
    }
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)address,&module) && module) {
        GetModuleFileNameA(module,name,sizeof(name));
        const char *base=strrchr(name,'\\');
        snprintf(out,size,"%s+0x%" PRIxPTR,base ? base+1 : name,address-(uintptr_t)module);
        return;
    }
    snprintf(out,size,"%p",(void *)address);
}
static int fatal_reported;
static LONG fatal_exception(EXCEPTION_POINTERS *info) {
    if (__atomic_exchange_n(&fatal_reported,1,__ATOMIC_ACQ_REL)) { Sleep(INFINITE); }
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    const CONTEXT *c=info->ContextRecord;
    char where[MAX_PATH+64];
    describe_address(where,sizeof(where),(uintptr_t)c->Rip);
    if (record->ExceptionCode==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2)
        fprintf(stderr,"Fault: %s access to %p at %s (thread %lu)\n",
                record->ExceptionInformation[0]==1 ? "write" : record->ExceptionInformation[0]==8 ? "execute" : "read",
                (void *)record->ExceptionInformation[1],where,GetCurrentThreadId());
    else
        fprintf(stderr,"Fault: exception 0x%08lx at %s (thread %lu)\n",record->ExceptionCode,where,GetCurrentThreadId());
    fprintf(stderr,"  rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n  rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n",
            c->Rax,c->Rbx,c->Rcx,c->Rdx,c->Rsi,c->Rdi,c->Rbp,c->Rsp);
    if (gpu_enabled) theatrhythm_gpu_dump_guest_writes(info);
    /* rbp chain (guest and host code keep frame pointers). */
    uintptr_t rbp=(uintptr_t)c->Rbp;
    for (int depth=0; depth<24 && rbp; ++depth) {
        uintptr_t frame[2];
        SIZE_T got=0;
        if (!ReadProcessMemory(GetCurrentProcess(),(void *)rbp,frame,sizeof(frame),&got) || got!=sizeof(frame) || frame[0]<=rbp) break;
        describe_address(where,sizeof(where),frame[1]);
        fprintf(stderr,"  #%d %s\n",depth,where);
        rbp=frame[0];
    }
    if (isa_table) isa_rewrite_report(isa_table,stderr);
    fflush(NULL);
    TerminateProcess(GetCurrentProcess(),3);
    return EXCEPTION_CONTINUE_SEARCH;
}
/* CONTEXT <-> IsaRegs (gpr in x86 numbering). */
static void isa_context_regs(CONTEXT *c, IsaRegs *regs, int to_context) {
    DWORD64 *g[16]={&c->Rax,&c->Rcx,&c->Rdx,&c->Rbx,&c->Rsp,&c->Rbp,&c->Rsi,&c->Rdi,
                    &c->R8,&c->R9,&c->R10,&c->R11,&c->R12,&c->R13,&c->R14,&c->R15};
    for (int i=0;i<16;++i) { if (to_context) *g[i]=regs->gpr[i]; else regs->gpr[i]=*g[i]; }
    if (to_context) c->EFlags=(DWORD)regs->rflags; else regs->rflags=c->EFlags;
}
/* THEATRHYTHM_FAULT_SITES=1 (diagnostics): every guest instruction that takes a GPU page-tracking
 * fault is printed the first time and at each power-of-two count, with the rsp alignment. */
static struct { volatile LONG64 key, count; } fault_sites[16384];
static void note_fault_site(const CONTEXT *c, int write) {
    static int enabled=-1;
    if (enabled<0) { const char *e=getenv("THEATRHYTHM_FAULT_SITES"); enabled=e && e[0]=='1'; }
    if (!enabled || !image) return;
    uint64_t offset=(uint64_t)c->Rip-(uint64_t)(uintptr_t)image;
    if (offset>=0x20000000) return;
    LONG64 key=(LONG64)(offset+1)|((LONG64)write<<62);
    for (uint64_t i=(offset*0x9e3779b97f4a7c15ull)>>50, n=0; n<16384; ++n, i=(i+1)&16383) {
        LONG64 seen=InterlockedCompareExchange64(&fault_sites[i].key,key,0);
        if (seen && seen!=key) continue;
        LONG64 count=InterlockedIncrement64(&fault_sites[i].count);
        if (!(count&(count-1)))
            fprintf(stderr,"Fault site: guest 0x%llx %s x%lld rsp%%16=%llu thread %lu\n",(unsigned long long)offset,
                    write ? "write" : "read",(long long)count,(unsigned long long)(c->Rsp&15),GetCurrentThreadId());
        return;
    }
}
/* THEATRHYTHM_REDZONE_PROBE=<offset>[,<offset>...] (tests, up to 4 guest image offsets): a hardware
 * execute breakpoint on each guest instruction, on its trampoline copy when the red-zone pass moved
 * it. The debug exception makes Windows write its exception frame below rsp there, as a GPU
 * page-tracking fault on that access would: red-zone data survives only where the pass moved the
 * access. Hits are printed at powers of two; for a function's first instruction, with the return
 * address. Every guest thread arms the breakpoints when it starts (runtime_orbis_fpu), through its
 * debug registers alone: continuing from an exception raised for that would come back with no
 * extended state in use, undoing THEATRHYTHM_DIRTY_ZMM. With it set, each hit also puts zmm31 back
 * in use in the state it returns with, as something can reset it on a thread: the next exception
 * then writes the full XSAVE frame. That frame ends at rsp rounded down to 64, so it reaches a slot
 * only when rsp is suitably aligned there (rsp-0x10: rsp % 64 == 8). Never probe a stack-relative
 * access: the pass does not move those, so the probe overwrites its red zone in any build. */
static unsigned char *probe_at[4];
static uint64_t probe_offset[4];
static volatile LONG64 probe_hits[4];
static int probe_count;
static ULONGLONG probe_start;
static int probe_dirty_zmm;
static void redzone_probe_arm(void) {
    CONTEXT c;
    memset(&c,0,sizeof c);
    c.ContextFlags=CONTEXT_DEBUG_REGISTERS;
    DWORD64 *dr[4]={&c.Dr0,&c.Dr1,&c.Dr2,&c.Dr3};
    for (int i=0;i<probe_count;++i) { *dr[i]=(DWORD64)(uintptr_t)probe_at[i]; c.Dr7|=1ull<<(2*i); } /* local, execute, 1 byte */
    if (!SetThreadContext(GetCurrentThread(),&c))
        fprintf(stderr,"Red-zone probe: arming thread %lu failed (%lu)\n",GetCurrentThreadId(),GetLastError());
}
/* zmm31 = all ones in the state an exception returns with (zmm16-30 zeroed if they were not in use). */
static void redzone_probe_dirty_zmm(CONTEXT *c) {
    DWORD64 mask;
    DWORD length=0;
    if ((c->ContextFlags&CONTEXT_XSTATE)!=CONTEXT_XSTATE || !GetXStateFeaturesMask(c,&mask)) return;
    unsigned char *hi16=LocateXStateFeature(c,XSTATE_AVX512_ZMM,&length);
    if (!hi16 || length<1024) return;
    if (!(mask&(1ull<<XSTATE_AVX512_ZMM))) memset(hi16,0,1024);
    memset(hi16+960,0xff,64);
    SetXStateFeaturesMask(c,mask|(1ull<<XSTATE_AVX512_ZMM));
}
static void redzone_probe_setup(void) {
    const char *list=getenv("THEATRHYTHM_REDZONE_PROBE");
    probe_start=GetTickCount64();
    const char *dirty=getenv("THEATRHYTHM_DIRTY_ZMM");
    probe_dirty_zmm=dirty && dirty[0]=='1' && __builtin_cpu_supports("avx512f");
    for (const char *p=list; p && *p && probe_count<4;) {
        char *end; uint64_t offset=strtoull(p,&end,16);
        if (end==p) break;
        unsigned char *copy=theatrhythm_redzone_copy(image+offset);
        probe_offset[probe_count]=offset;
        probe_at[probe_count]=copy ? copy : image+offset;
        printf("Red-zone probe %d: guest 0x%" PRIx64 " runs %s\n",probe_count,offset,copy ? "in a trampoline (moved)" : "in place");
        ++probe_count;
        p=*end==',' ? end+1 : end;
    }
    if (probe_count) runtime_thread_start_hook=redzone_probe_arm;
}
static LONG redzone_probe_exception(EXCEPTION_POINTERS *info) {
    CONTEXT *c=info->ContextRecord;
    for (int i=0;i<probe_count;++i) {
        if (!(c->Dr6&(1ull<<i)) || (unsigned char *)(uintptr_t)c->Rip!=probe_at[i]) continue;
        LONG64 n=InterlockedIncrement64(&probe_hits[i]);
        if (!(n&(n-1))) {
            uint64_t back=*(const uint64_t *)(uintptr_t)c->Rsp-(uint64_t)(uintptr_t)image;
            if (probe_at[i]==image+probe_offset[i] && back<0x20000000)
                fprintf(stderr,"Red-zone probe %d: guest 0x%" PRIx64 " x%lld at %.1f s, thread %lu, returns to guest 0x%" PRIx64 "\n",
                        i,probe_offset[i],(long long)n,(GetTickCount64()-probe_start)/1000.0,GetCurrentThreadId(),back);
            else fprintf(stderr,"Red-zone probe %d: guest 0x%" PRIx64 " x%lld at %.1f s, thread %lu\n",
                         i,probe_offset[i],(long long)n,(GetTickCount64()-probe_start)/1000.0,GetCurrentThreadId());
        }
        if (probe_dirty_zmm) redzone_probe_dirty_zmm(c);
        c->Dr6=0;
        c->EFlags|=0x10000; /* RF: run the instruction without breaking again */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG CALLBACK vectored_fault(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record=info->ExceptionRecord;
    DWORD code=record->ExceptionCode;
    if (probe_count && code==EXCEPTION_SINGLE_STEP &&
        redzone_probe_exception(info)==EXCEPTION_CONTINUE_EXECUTION)
        return EXCEPTION_CONTINUE_EXECUTION;
    if (code==EXCEPTION_BREAKPOINT && isa_table) {
        /* An int3 ISA site: emulated here; any other breakpoint goes on to the report below. */
        IsaRegs regs;
        uintptr_t rip;
        isa_context_regs(info->ContextRecord,&regs,0);
        if (isa_rewrite_trap(isa_table,(uintptr_t)record->ExceptionAddress,&regs,&rip)) {
            isa_context_regs(info->ContextRecord,&regs,1);
            info->ContextRecord->Rip=(DWORD64)rip;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code==EXCEPTION_ACCESS_VIOLATION && record->NumberParameters>=2) {
        /* GPU page tracking (write-protected guest pages) is resolved first. */
        if (gpu_enabled && theatrhythm_gpu_handle_fault(info,(void *)record->ExceptionInformation[1])) {
            note_fault_site(info->ContextRecord,record->ExceptionInformation[0]==1);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        if (runtime_fault_recover) {
            /* A speculative guest memory read (GPU draw preparation) faulted: the thread resumes
             * in theatrhythm_longjmp, outside the exception dispatcher, back at its recovery point. */
            unsigned long long *buffer=*runtime_fault_recover;
            runtime_fault_recover=NULL;
            CONTEXT *c=info->ContextRecord;
            c->Rsp=((c->Rsp-128)&~(DWORD64)15)-40;
            c->Rcx=(DWORD64)(uintptr_t)buffer;
            c->Rdx=1;
            c->Rip=(DWORD64)(uintptr_t)theatrhythm_longjmp;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    if (code==EXCEPTION_ACCESS_VIOLATION || code==EXCEPTION_ILLEGAL_INSTRUCTION || code==EXCEPTION_PRIV_INSTRUCTION ||
        code==EXCEPTION_STACK_OVERFLOW || code==EXCEPTION_INT_DIVIDE_BY_ZERO || code==EXCEPTION_DATATYPE_MISALIGNMENT) {
        /* Faults in guest code, the loader or the runtime end the process with a report; drivers
         * and system libraries may handle their own exceptions. */
        uintptr_t rip=(uintptr_t)info->ContextRecord->Rip;
        HMODULE self=GetModuleHandleA(NULL), module=NULL;
        int ours=(image && rip-(uintptr_t)image<0x20000000) ||
                 (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,(LPCSTR)rip,&module) && module==self) ||
                 !module;
        if (ours) return fatal_exception(info);
    }
    /* THEATRHYTHM_STRICT_HANDLES=1: a closed or invalid handle used anywhere is reported (diagnostics). */
    if (code==0xC0000008 /* STATUS_INVALID_HANDLE */) return fatal_exception(info);
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI unhandled_fault(EXCEPTION_POINTERS *info) { return fatal_exception(info); }
/* Guest code reads its TCB with `mov rax, gs:[0]` (link_*.py rewrote fs:[0]); on Windows
 * the displacement becomes the TEB TLS slot that holds the guest TCB (runtime_thread.c). */
uint32_t runtime_thread_tcb_offset(void);
static uint64_t patch_tcb_loads(const Segment *segments, uint64_t count) {
    static const unsigned char load[]={0x65,0x48,0x8b,0x04,0x25,0,0,0,0};
    uint32_t displacement=runtime_thread_tcb_offset();
    uint64_t patched=0;
    for (uint64_t s=0;s<count;++s) {
        if (!(segments[s].flags&1) || segments[s].size<sizeof(load)) continue;
        unsigned char *at=image+segments[s].address, *end=at+segments[s].size-sizeof(load);
        for (; at<=end; ++at)
            if (at[0]==0x65 && !memcmp(at,load,sizeof(load))) { memcpy(at+5,&displacement,4); ++patched; at+=sizeof(load)-1; }
    }
    return patched;
}
/* Guest red-zone protection (#12). Windows writes an exception's frame (CONTEXT and the
 * thread's live XSAVE state) directly below the faulting rsp, with no SysV red zone. When a guest
 * leaf function keeps locals there and one of its stores or loads takes a GPU page-tracking fault,
 * the frame overwrites them: on AVX2-only CPUs whenever the upper ymm halves are in use, on
 * AVX-512 ones when zmm16-31 are. theatrhythm_redzone_patch (shadPS4's static pass) moves each
 * memory access made while red-zone data is live to a trampoline that runs it with rsp 128 bytes
 * lower. Linux skips the red zone when it delivers a signal. THEATRHYTHM_REDZONE=0 turns it off. */
enum { REDZONE_TRAMPOLINES = 16 << 20 };
static int compare_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
/* Function starts (image offsets, sorted) from a module's .eh_frame_hdr binary search table. */
static uint64_t *redzone_function_starts(const unsigned char *image, uint64_t hdr, uint64_t *count) {
    const unsigned char *p = image + hdr;
    *count = 0;
    /* version 1, eh_frame_ptr sdata4/udata4, fde_count udata4, table datarel sdata4 */
    if (!hdr || p[0] != 1 || (p[1] & 0x0f) != 0x0b || p[2] != 0x03 || p[3] != 0x3b) return NULL;
    uint32_t n; memcpy(&n, p + 8, 4);
    uint64_t *starts = malloc((n ? n : 1) * sizeof(*starts));
    if (!starts) fail("allocation failed");
    for (uint32_t i = 0; i < n; ++i) {
        int32_t at; memcpy(&at, p + 12 + (size_t)i * 8, 4);
        starts[i] = hdr + (int64_t)at;
    }
    for (uint32_t i = 1; i < n; ++i) /* the table is sorted; keep it so if a tool did not */
        if (starts[i] < starts[i - 1]) { qsort(starts, n, sizeof(*starts), compare_u64); break; }
    *count = n;
    return starts;
}
/* 1 when [at, end) differs between the copy taken before the loader's own code patches and the
 * image, other than in the displacement of a TCB load (patch_tcb_loads keeps instruction bounds). */
static int redzone_changed(const unsigned char *before, const unsigned char *image, uint64_t at, uint64_t end) {
    static const unsigned char load[] = {0x65, 0x48, 0x8b, 0x04, 0x25};
    if (!memcmp(before + at, image + at, end - at)) return 0;
    for (uint64_t i = at; i < end; ++i) {
        if (before[i] == image[i]) continue;
        int displacement = 0;
        for (uint64_t j = i >= 8 ? i - 8 : 0; j + 5 <= i && !displacement; ++j)
            displacement = !memcmp(image + j, load, 5) && !memcmp(before + j, load, 5);
        if (!displacement) return 1;
    }
    return 0;
}
static void redzone_protect(unsigned char *image, uint64_t size, unsigned char *trampolines, const unsigned char *before,
                            const Segment *segments, uint64_t ns, const RuntimeModule *modules, uint64_t module_count,
                            const RuntimeModule *main_module) {
    const char *setting = getenv("THEATRHYTHM_REDZONE");
    if (setting && !strcmp(setting, "0")) { printf("Red zone: protection off (THEATRHYTHM_REDZONE=0)\n"); return; }
    ULONGLONG start = GetTickCount64();
    TheatrhythmRedZoneSegment *list = calloc(ns ? ns : 1, sizeof(*list));
    uint64_t **owned = calloc((module_count + 1) * 2, sizeof(*owned)), listed = 0, skipped = 0;
    if (!list || !owned) fail("allocation failed");
    for (uint64_t m = 0; m <= module_count; ++m) {
        const RuntimeModule *x = m < module_count ? &modules[m] : main_module;
        uint64_t count, skip_count = 0;
        uint64_t *starts = redzone_function_starts(image, x->eh_frame_hdr, &count);
        if (!starts) continue;
        uint64_t *skip = malloc((count ? count : 1) * sizeof(*skip));
        if (!skip) fail("allocation failed");
        owned[m * 2] = starts; owned[m * 2 + 1] = skip;
        for (uint64_t s = 0; s < ns; ++s) {
            uint64_t a = segments[s].address, e = a + segments[s].size;
            if (!(segments[s].flags & 1) || a < x->base || e > x->base + x->size) continue;
            for (uint64_t i = 0; i < count; ++i) {
                if (starts[i] < a || starts[i] >= e) continue;
                uint64_t end = i + 1 < count && starts[i + 1] < e ? starts[i + 1] : e;
                if (redzone_changed(before, image, starts[i], end)) skip[skip_count++] = starts[i];
            }
            list[listed++] = (TheatrhythmRedZoneSegment){a, segments[s].size, starts, count, skip, skip_count};
        }
        skipped += skip_count;
    }
    TheatrhythmRedZoneResult r;
    if (theatrhythm_redzone_patch(image, round_page(size), trampolines, REDZONE_TRAMPOLINES, list, listed, &r))
        fail("red-zone protection failed");
    printf("Red zone: %" PRIu64 " of %" PRIu64 " memory accesses with live red-zone data moved to trampolines "
           "(%" PRIu64 " KiB) in %" PRIu64 " functions; %" PRIu64 " functions changed by the loader left alone, "
           "%" PRIu64 " stack-relative, %" PRIu64 " control flow, %" PRIu64 " not relocatable, %" PRIu64 " functions with an "
           "unresolved jump table%s; %" PRIu64 " ms\n",
           r.patched_memory_instruction_count, r.memory_instruction_count, r.trampoline_bytes >> 10,
           r.red_zone_function_count, skipped, r.stack_dependent_memory_instruction_count,
           r.control_flow_memory_instruction_count, r.unrelocatable_memory_instruction_count,
           r.indirect_red_zone_function_count, r.trampoline_exhausted ? ", trampoline space exhausted" : "", (uint64_t)(GetTickCount64() - start));
    for (uint64_t i = 0; i < (module_count + 1) * 2; ++i) free(owned[i]);
    free(owned); free(list);
}
#endif
#ifndef _WIN32
/* ucontext gregs of IsaRegs.gpr (x86 numbering). */
static const int isa_gregs[16]={REG_RAX,REG_RCX,REG_RDX,REG_RBX,REG_RSP,REG_RBP,REG_RSI,REG_RDI,
                                REG_R8,REG_R9,REG_R10,REG_R11,REG_R12,REG_R13,REG_R14,REG_R15};
static void fault(int sig, siginfo_t *info, void *context) {
    if (sig == SIGTRAP && isa_table) {
        /* An int3 ISA site (RIP is past the CC byte); any other trap ends in the report below. */
        greg_t *g = ((ucontext_t *)context)->uc_mcontext.gregs;
        IsaRegs regs;
        uintptr_t rip;
        for (int i = 0; i < 16; ++i) regs.gpr[i] = (uint64_t)g[isa_gregs[i]];
        regs.rflags = (uint64_t)g[REG_EFL];
        if (isa_rewrite_trap(isa_table, (uintptr_t)g[REG_RIP] - 1, &regs, &rip)) {
            for (int i = 0; i < 16; ++i) g[isa_gregs[i]] = (greg_t)regs.gpr[i];
            g[REG_EFL] = (greg_t)regs.rflags;
            g[REG_RIP] = (greg_t)rip;
            return;
        }
    }
    /* GPU page tracking (write-protected guest pages) is resolved first. */
    if (gpu_enabled && sig == SIGSEGV && theatrhythm_gpu_handle_fault(context, info->si_addr)) return;
    /* A speculative guest memory read (runtime_memory.c) failed: resume its recovery point. */
    if ((sig == SIGSEGV || sig == SIGBUS) && runtime_fault_recover) {
        sigjmp_buf *recover = runtime_fault_recover;
        runtime_fault_recover = NULL;
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
        siglongjmp(*recover, 1);
    }
    /* The process is terminating: dladdr/snprintf are acceptable here. */
    ucontext_t *uc = context;
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    char line[512];
    Dl_info where;
    int site = isa_table ? isa_rewrite_site_of_stub(isa_table, rip) : -1;
    char emulating[64] = "";
    if (site >= 0)
        snprintf(emulating, sizeof(emulating), " While emulating site 0x%" PRIx64 " (%s)",
                 isa_table->sites[site].offset, isa_kind_name(isa_table->sites[site].kind));
    if (site >= 0 || rip - (uintptr_t)image < 0x10000000)
        snprintf(line, sizeof(line), "Fault: signal %d at guest offset 0x%lx, address %p%s\n",
                 sig, (unsigned long)(rip - (uintptr_t)image), info->si_addr, emulating);
    else if (dladdr((void *)rip, &where) && where.dli_fname)
        snprintf(line, sizeof(line), "Fault: signal %d in %s+0x%lx (%s), address %p\n", sig, where.dli_fname,
                 (unsigned long)(rip - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?", info->si_addr);
    else
        snprintf(line, sizeof(line), "Fault: signal %d at RIP %p, address %p\n", sig, (void *)rip, info->si_addr);
    { ssize_t written_=write(2, line, strlen(line)); (void)written_; }
    if (gpu_enabled) theatrhythm_gpu_dump_guest_writes(context);
    /* Host call chain (frames with unwind info; guest frames end it). */
    void *frames[32];
    int depth = backtrace(frames, 32);
    for (int i = 2; i < depth; ++i) {
        if (dladdr(frames[i], &where) && where.dli_fname)
            snprintf(line, sizeof(line), "  #%d %s+0x%lx (%s)\n", i, where.dli_fname,
                     (unsigned long)((uintptr_t)frames[i] - (uintptr_t)where.dli_fbase), where.dli_sname ? where.dli_sname : "?");
        else
            snprintf(line, sizeof(line), "  #%d %p\n", i, frames[i]);
        ssize_t written_=write(2, line, strlen(line)); (void)written_;
    }
    if (isa_table) { isa_rewrite_report(isa_table, stderr); fflush(stderr); }
    _exit(3); /* as the Windows handler */
}
#endif
#ifndef _WIN32
/* Watchdog: dump RIP and the rbp frame chain of every thread (guest offsets
 * when inside the image). Reads use process_vm_readv so bad frames cannot fault. */
static uintptr_t exe_base;
static void write_hex(char *out, uint64_t v) {
    const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) out[i] = digits[(v >> (60 - i * 4)) & 15];
}
static void dump_frames(ucontext_t *uc) {
    char line[] = "  tid=0000000000000000 rip=0000000000000000 image-relative=0000000000000000 host-relative=0000000000000000\n";
    uintptr_t rip=(uintptr_t)uc->uc_mcontext.gregs[REG_RIP], rbp=(uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
    uint64_t tid=(uint64_t)gettid();
    /* First argument register: the lock address when a thread waits on a futex. */
    char arg[]="  tid=0000000000000000 rdi=0000000000000000\n";
    write_hex(arg+6,tid); write_hex(arg+27,(uint64_t)uc->uc_mcontext.gregs[REG_RDI]);
    { ssize_t written_=write(2,arg,sizeof(arg)-1); (void)written_; }
    for (int depth=0; depth<24; ++depth) {
        write_hex(line+6,tid); write_hex(line+27,rip); write_hex(line+59,rip-(uintptr_t)image); write_hex(line+90,rip-exe_base);
        { ssize_t written_=write(2,line,sizeof(line)-1); (void)written_; }
        uintptr_t frame[2];
        struct iovec local={frame,sizeof(frame)}, remote={(void *)rbp,sizeof(frame)};
        if (!rbp || process_vm_readv(getpid(),&local,1,&remote,1,0)!=(ssize_t)sizeof(frame)) break;
        if (frame[0]<=rbp) break;
        rbp=frame[0]; rip=frame[1];
    }
}
static void thread_dump(int sig, siginfo_t *info, void *context) { (void)sig; (void)info; dump_frames(context); }
/* Every other thread prints its frames (SIGUSR2, thread_dump). `named`: its name and tid first
 * (not from a signal handler: snprintf is not async-signal-safe). */
static void dump_other_threads(int named) {
    int dir=open("/proc/self/task",O_RDONLY|O_DIRECTORY);
    char buffer[4096];
    long n;
    pid_t self=gettid();
    while (dir>=0 && (n=syscall(SYS_getdents64,dir,buffer,sizeof(buffer)))>0)
        for (long at=0; at<n;) {
            struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; } *d=(void *)(buffer+at);
            pid_t tid=(pid_t)strtol(d->name,NULL,10);
            if (tid>0 && tid!=self) {
                if (named) {
                    char path[64], comm[32]="?";
                    snprintf(path,sizeof(path),"/proc/self/task/%d/comm",(int)tid);
                    int fd=open(path,O_RDONLY);
                    ssize_t got=fd>=0 ? read(fd,comm,sizeof(comm)-1) : -1;
                    if (fd>=0) close(fd);
                    if (got>0) comm[got-(comm[got-1]=='\n')]=0;
                    fprintf(stderr,"Hang:   thread '%s' (tid %d, 0x%x):\n",comm,(int)tid,(unsigned)tid);
                }
                syscall(SYS_tgkill,getpid(),tid,SIGUSR2); usleep(20000);
            }
            at+=d->reclen;
        }
    if (dir>=0) close(dir);
}
static void watchdog(int sig, siginfo_t *info, void *context) {
    (void)info;
    const char head[]="STOP: watchdog timeout; thread stacks:\n";
    { ssize_t written_=write(2,head,sizeof(head)-1); (void)written_; }
    dump_frames(context);
    dump_other_threads(0);
    usleep(100000);
    _exit(128 + sig);
}
static void hang_dump(unsigned seconds) {
    fprintf(stderr,"Hang: no new frame for %u s; each thread's frames follow (image-relative: guest offset, "
            "host-relative: offset in the exe; THEATRHYTHM_HANG_DUMP=0 turns this off)\n",seconds);
    dump_other_threads(1);
    usleep(100000);
    fprintf(stderr,"Hang: end of thread stacks\n");
}
#else
/* A thread's stack is walked on a copy taken while it is suspended: a running thread's stack
 * changes under a live walk, and RtlVirtualUnwind then follows a stale frame register into
 * unmapped memory (the --timeout watchdog faulted in ntdll on a busy draw-preparation thread).
 * Only the copy runs while the thread is suspended (no locks: the thread may hold the heap or
 * loader lock); unwind lookups run after it resumes. The copy holds the top STACK_COPY bytes from
 * rsp, then STACK_SLACK zero bytes, so unwind reads a little past its end find zeros (rip 0 ends a
 * walk). Pointers into the copied range, in the copy and in the registers, are moved to the copy.
 * dump_lock: the hang dump and the watchdog share the buffer. */
enum { HANG_FRAMES = 24, STACK_COPY = 1<<20, STACK_SLACK = 64<<10 };
static uint64_t stack_copy[(STACK_COPY+STACK_SLACK)/8];
static SRWLOCK dump_lock = SRWLOCK_INIT;
/* With the thread suspended: copies [rsp & ~15, end of its committed region) up to STACK_COPY
 * bytes; returns the size (0: not readable) and the copied range's start. */
static size_t copy_stack(const CONTEXT *c, uintptr_t *from) {
    MEMORY_BASIC_INFORMATION info;
    const uintptr_t low=(uintptr_t)c->Rsp&~(uintptr_t)15;
    if (!low || !VirtualQuery((void *)low,&info,sizeof(info)) || info.State!=MEM_COMMIT ||
        (info.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
        !(info.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))
        return 0;
    size_t size=(uintptr_t)info.BaseAddress+info.RegionSize-low;
    if (size>STACK_COPY) size=STACK_COPY;
    __builtin_memcpy(stack_copy,(const void *)low,size);
    *from=low;
    return size;
}
/* After the thread resumed: zeroes the slack and moves every stack pointer to the copy. */
static void rebase_stack_copy(CONTEXT *c, uintptr_t from, size_t size) {
    const uintptr_t to=(uintptr_t)stack_copy, end=from+size;
    memset((char *)stack_copy+size,0,STACK_SLACK);
    for (size_t i=0;i<size/8;++i)
        if (stack_copy[i]>=from && stack_copy[i]<end) stack_copy[i]=stack_copy[i]-from+to;
    DWORD64 *g[16]={&c->Rax,&c->Rcx,&c->Rdx,&c->Rbx,&c->Rsp,&c->Rbp,&c->Rsi,&c->Rdi,
                    &c->R8,&c->R9,&c->R10,&c->R11,&c->R12,&c->R13,&c->R14,&c->R15};
    for (int i=0;i<16;++i) if (*g[i]>=from && *g[i]<end) *g[i]=*g[i]-from+to;
}
/* Frames of a thread from its registers on the stack copy [low, high): host code unwinds through
 * its .pdata (a function without any is a leaf: the return address is at rsp), guest code through
 * its rbp chain. Each step starts inside the copy; a read that faults anyway (a frame register
 * that unwind data trusts but the stack does not hold) ends the walk (runtime_fault_recover), as
 * does a step that leaves the copy. */
static int thread_frames(CONTEXT *c, uintptr_t *out, uintptr_t low, uintptr_t high) {
    volatile int n=0;
    sigjmp_buf recover;
    if (sigsetjmp(recover,0)) return n; /* the handler cleared runtime_fault_recover */
    while (n<HANG_FRAMES && c->Rip) {
        out[n]=(uintptr_t)c->Rip; n=n+1;
        const DWORD64 rsp=c->Rsp;
        if (rsp<low || rsp+8>high) break;
        DWORD64 base=0;
        const int guest=image && c->Rip-(uintptr_t)image<0x20000000;
        PRUNTIME_FUNCTION function=guest ? NULL : RtlLookupFunctionEntry(c->Rip,&base,NULL);
        runtime_fault_recover=&recover;
        if (function) {
            void *handler_data; DWORD64 establisher;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER,base,c->Rip,function,c,&handler_data,&establisher,NULL);
        } else if (guest) {
            if (c->Rbp<rsp || c->Rbp+16>high) { runtime_fault_recover=NULL; break; }
            const DWORD64 *frame=(const DWORD64 *)(uintptr_t)c->Rbp;
            c->Rip=frame[1]; c->Rsp=c->Rbp+16; c->Rbp=frame[0];
        } else {
            c->Rip=*(const DWORD64 *)(uintptr_t)rsp; c->Rsp=rsp+8;
        }
        runtime_fault_recover=NULL;
        if (c->Rsp<=rsp) break;
    }
    return n;
}
/* Every other thread: name, tid, CPU time and frames, one line each. A thread is suspended while
 * its registers and stack are copied (copy_stack), then walked on the copy. */
static void dump_threads(void) {
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);
    if (snap==INVALID_HANDLE_VALUE) return;
    AcquireSRWLockExclusive(&dump_lock);
    THREADENTRY32 entry={.dwSize=sizeof(entry)};
    const DWORD pid=GetCurrentProcessId(), self=GetCurrentThreadId();
    for (BOOL ok=Thread32First(snap,&entry);ok;ok=Thread32Next(snap,&entry)) {
        if (entry.th32OwnerProcessID!=pid || entry.th32ThreadID==self) continue;
        HANDLE h=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ThreadID);
        if (!h) continue;
        CONTEXT c;
        memset(&c,0,sizeof(c));
        c.ContextFlags=CONTEXT_FULL;
        BOOL got=FALSE;
        uintptr_t from=0;
        size_t copied=0;
        if (SuspendThread(h)!=(DWORD)-1) {
            got=GetThreadContext(h,&c);
            if (got) copied=copy_stack(&c,&from);
            ResumeThread(h);
        }
        if (copied) rebase_stack_copy(&c,from,copied);
        char name[64]="";
        PWSTR wide=NULL;
        if (SUCCEEDED(GetThreadDescription(h,&wide)) && wide) {
            WideCharToMultiByte(CP_UTF8,0,wide,-1,name,sizeof(name),NULL,NULL);
            LocalFree(wide);
        }
        FILETIME created,exited,kernel,user;
        double ms=0;
        if (GetThreadTimes(h,&created,&exited,&kernel,&user))
            ms=(double)((((uint64_t)kernel.dwHighDateTime<<32)|kernel.dwLowDateTime)+(((uint64_t)user.dwHighDateTime<<32)|user.dwLowDateTime))/1e4;
        CloseHandle(h);
        uintptr_t frames[HANG_FRAMES];
        int n=0;
        if (copied) n=thread_frames(&c,frames,(uintptr_t)stack_copy,(uintptr_t)stack_copy+copied);
        else if (got) frames[n++]=(uintptr_t)c.Rip;
        char line[HANG_FRAMES*(MAX_PATH+8)+256], where[MAX_PATH+64];
        int used=snprintf(line,sizeof(line),"Hang:   '%s' (tid %lu, %.0f ms CPU):",name[0] ? name : "?",entry.th32ThreadID,ms);
        for (int i=0;i<n && used>0 && (size_t)used<sizeof(line);++i) {
            describe_address(where,sizeof(where),frames[i]);
            used+=snprintf(line+used,sizeof(line)-(size_t)used,"%s %s",i ? " <" : "",where);
        }
        if (!n) snprintf(line+used,sizeof(line)-(size_t)used," (no registers)");
        fprintf(stderr,"%s\n",line);
    }
    ReleaseSRWLockExclusive(&dump_lock);
    CloseHandle(snap);
}
static void hang_dump(unsigned seconds) {
    fprintf(stderr,"Hang: no new frame for %u s; each thread's frames follow, innermost first "
            "(THEATRHYTHM_HANG_DUMP=0 turns this off)\n",seconds);
    dump_threads();
    fprintf(stderr,"Hang: end of thread stacks\n");
    fflush(NULL);
}
static DWORD WINAPI watchdog_thread(void *seconds) {
    Sleep(*(unsigned *)seconds*1000u);
    fprintf(stderr,"STOP: watchdog timeout after %u s; thread stacks:\n",*(unsigned *)seconds);
    dump_threads();
    runtime_report();
    fflush(NULL);
    _exit(128+14);
}
#endif
/* Hang dump (THEATRHYTHM_HANG_DUMP=seconds, default 20, 0 off): once a frame has been shown, a
 * picture held that long logs every thread's frames, once until frames move again. Players' logs
 * then show where a frozen game waits. */
static unsigned hang_seconds;
static void hang_watch_loop(void) {
    unsigned long long last=0;
    unsigned held=0;
    int dumped=0;
    for (;;) {
#ifdef _WIN32
        Sleep(1000);
#else
        sleep(1);
#endif
        const unsigned long long shown=theatrhythm_gpu_flips();
        if (shown!=last) { last=shown; held=0; dumped=0; continue; }
        if (!shown || dumped || ++held<hang_seconds) continue;
        hang_dump(hang_seconds);
        dumped=1;
    }
}
#ifdef _WIN32
static DWORD WINAPI hang_watch(void *unused) { (void)unused; SetThreadDescription(GetCurrentThread(),L"theatrhythm:HangWatch"); hang_watch_loop(); return 0; }
#else
static void *hang_watch(void *unused) { (void)unused; pthread_setname_np(pthread_self(),"HangWatch"); hang_watch_loop(); return NULL; }
#endif
static void start_hang_watch(void) {
    const char *env=getenv("THEATRHYTHM_HANG_DUMP");
    hang_seconds=env && *env ? (unsigned)strtoul(env,NULL,10) : 20;
    if (!hang_seconds) return;
#ifdef _WIN32
    HANDLE h=CreateThread(NULL,0,hang_watch,NULL,0,NULL);
    if (h) CloseHandle(h);
#else
    pthread_t thread;
    if (!pthread_create(&thread,NULL,hang_watch,NULL)) pthread_detach(thread);
#endif
}
/* param.sfo lookup: string or integer value of key, 0 when absent. */
static int sfo_value(const char *path, const char *key, char *text, size_t text_size, uint32_t *number) {
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    unsigned char data[65536];
    size_t n=fread(data,1,sizeof(data),f); fclose(f);
    if (n<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t keys, values, count;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&count,data+16,4);
    for (uint32_t i=0;i<count && 20+i*16+16<=n;++i) {
        const unsigned char *e=data+20+i*16;
        uint16_t key_offset, format; uint32_t length, offset;
        memcpy(&key_offset,e,2); memcpy(&format,e+2,2); memcpy(&length,e+4,4); memcpy(&offset,e+12,4);
        if (keys+key_offset>=n || values+offset+length>n || strcmp((const char *)data+keys+key_offset,key)) continue;
        if (format==0x0404 && number && length>=4) { memcpy(number,data+values+offset,4); return 1; }
        if (text && text_size) {
            size_t copy=length<text_size-1 ? length : text_size-1;
            memcpy(text,data+values+offset,copy); text[copy]=0;
        }
        return 1;
    }
    return 0;
}
static int mapped(Segment *segments, uint64_t count, uint64_t address, uint64_t bytes) {
    for (uint64_t i = 0; i < count; ++i)
        if (address >= segments[i].address && bytes <= segments[i].size &&
            address - segments[i].address <= segments[i].size - bytes) return 1;
    return 0;
}
/* Restarts the game (sceSystemServiceLoadExec of its own eboot): this process's command line is
 * started again, then this process ends, releasing its GPU device and memory. */
#ifndef _WIN32
/* Linux: the same process image again (exec releases the GPU device and the guest memory). */
static char **restart_argv;
void runtime_restart(void) {
    fflush(NULL);
    puts("Runtime: restarting with the same command line");
    execv("/proc/self/exe",restart_argv);
    perror("runtime_restart: execv");
    _exit(1);
}
#else
void runtime_restart(void) {
    fflush(NULL);
    puts("Runtime: restarting with the same command line");
    char *line=_strdup(GetCommandLineA());
    /* Same output handles: run.py, reading the game's log, keeps reading the new launch. */
    STARTUPINFOA startup={.cb=sizeof(startup),.dwFlags=STARTF_USESTDHANDLES,
                          .hStdInput=GetStdHandle(STD_INPUT_HANDLE),.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE),
                          .hStdError=GetStdHandle(STD_ERROR_HANDLE)};
    PROCESS_INFORMATION process;
    if (!line || !CreateProcessA(NULL,line,NULL,NULL,TRUE,0,NULL,NULL,&startup,&process)) {
        fprintf(stderr,"runtime_restart: CreateProcess failed (%lu)\n",GetLastError()); _exit(1);
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    TerminateProcess(GetCurrentProcess(),0);
}
#endif

/* The guest code needs the PS4's instruction sets; THEATRHYTHM_CPU_CHECK=0 skips this. A
 * missing fatal set prints STOP and returns 0; the rewritable ones go to cpu_missing_sets, named
 * in missing. */
static int cpu_supported(char *missing, size_t size) {
    missing[0] = 0;
    const char *check = getenv("THEATRHYTHM_CPU_CHECK");
    if (check && !strcmp(check, "0")) return 1;
    const CpuFeatureWords words = cpu_feature_words();
    if (cpu_missing_fatal(words, missing, size)) {
        printf("STOP: this CPU lacks instruction sets the game's code uses: %s "
               "(Intel Sandy Bridge 2011 or newer, or AMD FX or newer, is needed)\n", missing);
        return 0;
    }
    cpu_missing_sets = cpu_missing_rewritable(words, missing, size);
    return 1;
}

/* settings.ini button_icons: the icon and pad files scripts/button_icons.py and
 * scripts/controller_pad.py wrote next to boot.bin. */
static void configure_button_icons(const char *boot) {
    const char *slash=strrchr(boot,'/'), *back=strrchr(boot,'\\');
    if (back && (!slash || back>slash)) slash=back;
    char icons[512], pad[512];
    snprintf(icons,sizeof(icons),"%.*sbutton_icons",slash ? (int)(slash-boot+1) : 0,boot);
    snprintf(pad,sizeof(pad),"%.*scontroller_pad",slash ? (int)(slash-boot+1) : 0,boot);
    runtime_button_icons_configure(icons,pad,theatrhythm_button_icons());
}

/* Display Settings icon in the Options menu: the Config atlas bundle scripts/config_icon.py wrote
 * next to boot.bin, served in place of the game's at both paths the game may open it by. 1 when
 * mounted. */
static int mount_config_icon(const char *boot) {
    static const char bundle[]="Media/StreamingAssets/aa/PS4/0aecbe09e3673328eba2f60a0720e8b5.bundle";
    static const char *const prefixes[]={"/app0/","/archive/mount/point/"};
    const char *slash=strrchr(boot,'/'), *back=strrchr(boot,'\\');
    if (back && (!slash || back>slash)) slash=back;
    char host[512], guest[128];
    snprintf(host,sizeof(host),"%.*sconfig_icon/display.bundle",slash ? (int)(slash-boot+1) : 0,boot);
    FILE *f=fopen(host,"rb");
    if (!f) { printf("Config icon: %s missing (python run.py prepare); Display Settings shows Logo Usage's icon\n",host); return 0; }
    fclose(f);
    for (size_t i=0;i<sizeof(prefixes)/sizeof(*prefixes);++i) {
        snprintf(guest,sizeof(guest),"%s%s",prefixes[i],bundle);
        if (runtime_file_mount(guest,host)) { printf("Config icon: cannot mount %s; Display Settings shows Logo Usage's icon\n",host); return 0; }
    }
    printf("Config icon: %s\n",host);
    return 1;
}

/* render_resolution=4k: the game's Media/boot.config with platform-ps4-videoout-width=3840, written
 * next to boot.bin as boot_4k.config and served in its place. In PS4 Pro mode Unity then registers
 * 3840x2160 display buffers and renders at that size; without the key it keeps 1080p. The game
 * opens it as /app0/media/boot.config. 1 when mounted. */
static int mount_boot_config_4k(const char *boot,const char *app0) {
    static const char *const guests[]={"/app0/Media/boot.config","/app0/media/boot.config",
        "/archive/mount/point/Media/boot.config","/archive/mount/point/media/boot.config"};
    static const char key[]="platform-ps4-videoout-width=";
    const char *slash=strrchr(boot,'/'), *back=strrchr(boot,'\\');
    if (back && (!slash || back>slash)) slash=back;
    char source[600], host[512], line[512];
    snprintf(source,sizeof(source),"%s/Media/boot.config",app0);
    snprintf(host,sizeof(host),"%.*sboot_4k.config",slash ? (int)(slash-boot+1) : 0,boot);
    FILE *in=fopen(source,"rb"), *out=in ? fopen(host,"wb") : NULL;
    if (!out) {
        if (in) fclose(in);
        printf("Render: cannot write %s from %s; the game renders at 1080p\n",host,source);
        return 0;
    }
    while (fgets(line,sizeof(line),in)) {
        if (strncmp(line,key,sizeof(key)-1)) fputs(line,out);
        if (line[0] && line[strlen(line)-1]!='\n') fputc('\n',out);
    }
    fprintf(out,"%s3840\n",key);
    fclose(in);
    int failed=fclose(out)!=0;
    for (size_t i=0;!failed && i<sizeof(guests)/sizeof(*guests);++i) failed=runtime_file_mount(guests[i],host)!=0;
    if (failed) { printf("Render: cannot serve %s; the game renders at 1080p\n",host); return 0; }
    printf("Render: 3840x2160 in PS4 Pro mode (%s)\n",host);
    return 1;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
#ifndef _WIN32
    restart_argv=argv;
    /* Keep host heap objects handed to the guest (thread handles, TLS) in the
       non-PIE brk heap, i.e. below 1 TiB: the guest packs pointers into 40 bits. */
    mallopt(M_ARENA_MAX,1);
    mallopt(M_MMAP_THRESHOLD,32*1024*1024);
#endif
    if (argc == 2 && !strcmp(argv[1], "--vulkan-only")) return vulkan_smoke();
    char missing[128];
    if (argc == 2 && !strcmp(argv[1], "--cpu-features")) {
        if (!cpu_supported(missing, sizeof missing)) return 20;
        if (cpu_missing_sets) printf("CPU: missing %s\n", missing);
        else puts("CPU: complete");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--gamepad")) {
        /* Before the memory reservation: this only asks SDL, nothing of the game is set up yet. */
        runtime_pad_describe();
        return 0;
    }
    /* No boot file (a shortcut or a double-click): the set-up folders from paths.ini (src/launch.c). */
    if ((argc < 2 || !strncmp(argv[1], "--", 2)) && launch_from_paths_ini(&argc, &argv)) return 1;
#ifdef _WIN32
    /* The PS4 user range must stay free of host allocations (GPU driver, heaps), and the guest
     * TCB needs one of the 64 TEB TLS slots before DLLs (Vulkan drivers) take them. */
    runtime_memory_reserve();
    runtime_thread_tcb_offset();
#endif
    int cpu_only = 0, strict_imports = 0, check_imports = 0, monitor_icon = 0;
    unsigned timeout_seconds = 10;
    const char *content_profile=NULL, *app0=NULL, *user_dir=NULL, *isa_file=NULL, *dlc_dir=NULL;
    const char *mounts[24]; int mount_count=0;  /* --mount guest=host: one game file or folder each */
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--cpu-only")) cpu_only = 1;
        else if (!strcmp(argv[i], "--strict-imports")) strict_imports = 1;
        else if (!strcmp(argv[i], "--check-imports")) check_imports = 1;
        else if (!strcmp(argv[i], "--content-profile") && i+1<argc) content_profile=argv[++i];
        else if (!strcmp(argv[i], "--app0") && i+1<argc) app0=argv[++i];
        else if (!strcmp(argv[i], "--user") && i+1<argc) user_dir=argv[++i];
        else if (!strcmp(argv[i], "--dlc") && i+1<argc) dlc_dir=argv[++i];
        else if (!strcmp(argv[i], "--isa-rewrite") && i+1<argc) isa_file=argv[++i];
        else if (!strcmp(argv[i], "--mount") && i+1<argc) {
            /* guest=host; the guest part never has '=', so the first one splits */
            const char *spec=argv[++i], *eq=strchr(spec,'=');
            if (mount_count==24 || !eq || eq==spec || !eq[1]) { fprintf(stderr, "Bad --mount (guest=host, at most 24): %s\n", spec); return 1; }
            mounts[mount_count++]=spec;
        }
        else if (!strcmp(argv[i], "--timeout") && i+1<argc) timeout_seconds=(unsigned)strtoul(argv[++i],NULL,10);
        else { fprintf(stderr, "Unknown option: %s\n", argv[i]); return 1; }
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s boot.bin [--cpu-only] [--strict-imports] [--check-imports] [--content-profile file] [--app0 dir] [--user dir] [--dlc dir] [--isa-rewrite file] [--mount guest=host]... [--timeout seconds] | --vulkan-only | --cpu-features | --gamepad\n", argv[0]);
        return 1;
    }
    if (!cpu_supported(missing, sizeof missing)) return 20;
    if (cpu_missing_sets) printf("CPU: missing %s\n", missing);
    /* THEATRHYTHM_ISA_REWRITE=0 never rewrites, =1 rewrites every site; by default the sets the CPU lacks. */
    const char *policy = getenv("THEATRHYTHM_ISA_REWRITE");
    unsigned isa_sets = policy && !strcmp(policy, "1") ? 0xffffffffu : policy && !strcmp(policy, "0") ? 0 : cpu_missing_sets;
    if (cpu_missing_sets && !isa_sets) {
        printf("STOP: this CPU lacks %s and the rewrite is disabled (THEATRHYTHM_ISA_REWRITE=0)\n", missing);
        return 20;
    }
    if (cpu_missing_sets && !isa_file) {
        printf("STOP: this CPU lacks %s and no rewrite table matches the image\n", missing);
        return 20;
    }
    if (content_profile) {
        FILE *profile=fopen(content_profile,"rb");
        unsigned char data[28];
        if (!profile) fail("cannot open content profile");
        if (fread(data,1,sizeof(data),profile)!=sizeof(data) || fgetc(profile)!=EOF || memcmp(data,"TFCONT01",8)) fail("invalid content profile");
        fclose(profile);
        uint32_t values[5];
        for (unsigned i=0;i<5;++i) {
            unsigned char *p=data+8+i*4;
            values[i]=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
        }
        runtime_content_configure(values);
    }
    if (app0) {
        runtime_file_configure(app0, user_dir ? user_dir : "user");
        if (user_dir && user_dir[0]) {
#ifdef _WIN32
            _putenv_s("THEATRHYTHM_USER_DIR", user_dir);
#else
            setenv("THEATRHYTHM_USER_DIR", user_dir, 1);
#endif
        }
        for (int i = 0; i < mount_count; ++i) {
            const char *eq = strchr(mounts[i], '=');
            char guest[128];
            size_t glen = (size_t)(eq - mounts[i]);
            if (glen >= sizeof(guest)) { fprintf(stderr, "Cannot mount %s: guest path longer than 127 characters\n", mounts[i]); return 1; }
            memcpy(guest, mounts[i], glen); guest[glen] = 0;
            if (runtime_file_mount(guest, eq + 1)) { fprintf(stderr, "Cannot mount %s: too many mounts or host path longer than 511 characters\n", mounts[i]); return 1; }
            printf("Runtime: mount %s -> %s\n", guest, eq + 1);
        }
        char sfo[4096], id[16]="", title_id[16]="", version[16]="";
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0);
        if (sfo_value(sfo,"INSTALL_DIR_SAVEDATA",id,sizeof(id),NULL) || sfo_value(sfo,"TITLE_ID",id,sizeof(id),NULL))
            runtime_savedata_configure(id);
        sfo_value(sfo,"TITLE_ID",title_id,sizeof(title_id),NULL);
        sfo_value(sfo,"APP_VER",version,sizeof(version),NULL); /* the patch's version (VERSION stays the disc's) */
        runtime_game_identify(title_id,version,app0,argv[1]);
        if (dlc_dir && dlc_dir[0]) runtime_content_extra_dir(dlc_dir, title_id[0] ? title_id : NULL, app0);
    }
    theatrhythm_gpu_register_kernel();
#ifdef _WIN32
    SYSTEM_INFO system_info; GetSystemInfo(&system_info); page_size = system_info.dwPageSize;
    AddVectoredExceptionHandler(1, vectored_fault);
    SetUnhandledExceptionFilter(unhandled_fault);
    if (getenv("THEATRHYTHM_STRICT_HANDLES")) {
        PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY strict={0};
        strict.RaiseExceptionOnInvalidHandleReference=1;
        strict.HandleExceptionsPermanentlyEnabled=1;
        if (!SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy,&strict,sizeof(strict)))
            fprintf(stderr,"THEATRHYTHM_STRICT_HANDLES: policy not set (%lu)\n",GetLastError());
    }
    /* Watchdog: after --timeout seconds print every thread's frames and the runtime report and
     * end, as SIGALRM does below. 0 disables it. */
    static unsigned watchdog_seconds;
    watchdog_seconds=timeout_seconds;
    if (watchdog_seconds) CloseHandle(CreateThread(NULL,0,watchdog_thread,&watchdog_seconds,0,NULL));
#else
    page_size = (size_t)sysconf(_SC_PAGESIZE);
    struct sigaction sa = {0}; sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL); /* int3 ISA sites */
    Dl_info self_info;
    if (dladdr((void *)main,&self_info)) exe_base=(uintptr_t)self_info.dli_fbase;
    struct sigaction dump = {0}; dump.sa_sigaction = thread_dump; dump.sa_flags = SA_SIGINFO|SA_RESTART;
    sigemptyset(&dump.sa_mask); sigaction(SIGUSR2, &dump, NULL);
    struct sigaction alarm_action = {0}; alarm_action.sa_sigaction = watchdog; alarm_action.sa_flags = SA_SIGINFO;
    sigemptyset(&alarm_action.sa_mask); sigaction(SIGALRM, &alarm_action, NULL);
    alarm(timeout_seconds); /* 0 disables the watchdog */
#endif
    FILE *f = fopen(argv[1], "rb");
    if (!f) fail("cannot open boot file; run prepare.py first");
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || (memcmp(magic, "TFPROBE1", 8) && memcmp(magic, "TFPROBE2", 8) && memcmp(magic,"TFPROBE3",8) && memcmp(magic,"TFPROBE4",8) && memcmp(magic,"TFPROBE5",8) && memcmp(magic,"TFPROBE6",8) && memcmp(magic,"TFPROBE7",8))) fail("bad boot file signature");
    uint64_t size = read64(f), entry = read64(f), ns = read64(f), nr = read64(f);
    import_count = read64(f);
    uint64_t capabilities = memcmp(magic, "TFPROBE1", 8) ? read64(f) : 0;
    if (capabilities & ~UINT64_C(1)) fail("unknown runtime capabilities");
    runtime_start(strict_imports ? 0 : capabilities);
    if (!size || size > 512*1024*1024 || entry >= size || !ns || ns > 64 || nr > 1000000 || import_count > 100000)
        fail("boot file limits exceeded");
    int unwind=!memcmp(magic,"TFPROBE7",8);
    int named=!memcmp(magic,"TFPROBE6",8) || unwind;
    int multi=!memcmp(magic,"TFPROBE5",8) || named;
    int linked=!memcmp(magic,"TFPROBE3",8) || !memcmp(magic,"TFPROBE4",8) || multi;
    int native_libc=linked && !strict_imports && (capabilities&1);
    uint64_t main_tls[4]={0};
    uint64_t nb=0,procparam=0;
    uint64_t *bindings=calloc(import_count ? import_count : 1,sizeof(*bindings));
    uint64_t *binding_kinds=calloc(import_count ? import_count : 1,sizeof(*binding_kinds));
    if (!bindings || !binding_kinds) fail("allocation failed");
    if (multi) {
        /* TFPROBE5: procparam, eboot TLS, module table, bindings (link_modules.py). TFPROBE6 adds
         * each module's flags, procparam and file name, and the modules' exports after the bindings. */
        procparam=read64(f);
        for (int i=0;i<4;++i) main_tls[i]=read64(f);
        module_count=read64(f);
        if (!module_count || module_count>sizeof(modules)/sizeof(*modules)) fail("invalid module count");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            x->base=read64(f); x->size=read64(f); x->init=read64(f); x->tls_address=read64(f);
            x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=read64(f);
            if (named) {
                x->flags=read64(f); x->procparam=read64(f);
                if (fread(x->name,1,sizeof(x->name),f)!=sizeof(x->name) || !memchr(x->name,0,sizeof(x->name))) fail("bad module name");
            } else if (m==0) snprintf(x->name,sizeof(x->name),"libc.prx");
        }
        nb=read64(f);
    } else if (linked) {
        LinkedModule *x=&modules[0];
        module_count=1;
        x->base=read64(f); x->size=read64(f); x->init=read64(f);
        x->tls_address=read64(f); x->tls_memsz=read64(f); x->tls_filesz=read64(f); x->tls_module=2; nb=read64(f);
        procparam=read64(f);
        if (!memcmp(magic,"TFPROBE4",8)) for (int i=0;i<4;++i) main_tls[i]=read64(f);
    }
    if (linked) {
        if (main_tls[1]>main_tls[2] || main_tls[2]>1024*1024 || main_tls[0]>size ||
            main_tls[1]>size-main_tls[0] || (main_tls[3] & (main_tls[3]-1)) || main_tls[3]>4096)
            fail("invalid eboot TLS metadata");
        for (uint64_t m=0;m<module_count;++m) {
            LinkedModule *x=&modules[m];
            if (x->base>=size || !x->size || x->size>size-x->base || x->init<x->base || x->init-x->base>=x->size ||
                (x->tls_module && (x->tls_address>=size || x->tls_memsz>size-x->tls_address || x->tls_memsz>1024*1024 ||
                                   x->tls_filesz>x->tls_memsz || x->tls_module<2 || x->tls_module>=RUNTIME_TLS_MODULES)) ||
                (x->flags & ~(uint64_t)RUNTIME_MODULE_DEFERRED) || (x->procparam && (x->procparam<x->base || x->procparam-x->base>=x->size)))
                fail("invalid linked module metadata");
        }
        if (nb>import_count) fail("invalid binding count");
        for (uint64_t i=0;i<nb;++i) {
            uint64_t index=read64(f),address=read64(f),kind=read64(f);
            int inside=0;
            for (uint64_t m=0;m<module_count;++m)
                if (address>=modules[m].base && address-modules[m].base<modules[m].size) inside=1;
            if (index>=import_count || !inside || (kind!=1 && kind!=2) || bindings[index]) fail("invalid native binding");
            bindings[index]=address; binding_kinds[index]=kind;
        }
        if (named) {
            /* Exports grouped by module in table order: (module, NID, address, kind). */
            uint64_t ne=read64(f), previous=0;
            if (ne>1000000) fail("invalid export count");
            RuntimeModuleExport *exports=calloc(ne ? ne : 1,sizeof(*exports));
            if (!exports) fail("allocation failed");
            for (uint64_t i=0;i<ne;++i) {
                uint64_t index=read64(f);
                RuntimeModuleExport *e=&exports[i];
                if (fread(e->nid,1,sizeof(e->nid),f)!=sizeof(e->nid)) fail("truncated boot file");
                e->address=read64(f); e->kind=read64(f);
                if (index>=module_count || index<previous || !memchr(e->nid,0,sizeof(e->nid)) || (e->kind!=1 && e->kind!=2) ||
                    e->address<modules[index].base || e->address-modules[index].base>=modules[index].size)
                    fail("invalid module export");
                if (!modules[index].exports) modules[index].exports=e;
                ++modules[index].export_count; previous=index;
            }
        }
        if (unwind) {
            /* TFPROBE7: unwind fields of each module in table order, then of the eboot. */
            if (read64(f)!=module_count+1) fail("invalid unwind table");
            for (uint64_t m=0;m<=module_count;++m) {
                RuntimeModule *x=m<module_count ? &modules[m] : &main_module;
                x->eh_frame_hdr=read64(f); x->eh_frame=read64(f); x->eh_frame_size=read64(f);
                x->seg0=read64(f); x->seg0_size=read64(f);
                if (x->eh_frame_hdr>=size || x->eh_frame>=size || x->eh_frame_size>size-x->eh_frame ||
                    x->seg0>=size || x->seg0_size>size-x->seg0) fail("invalid unwind metadata");
            }
            main_module.size=modules[0].base;
            snprintf(main_module.name,sizeof(main_module.name),"eboot.bin");
        }
    }
    Segment *segments = calloc(ns, sizeof(*segments));
    Reloc *relocs = calloc(nr ? nr : 1, sizeof(*relocs));
    names = calloc(import_count ? import_count : 1, sizeof(*names));
    if (!segments || !relocs || !names) fail("allocation failed");
    for (uint64_t i = 0; i < ns; ++i) {
        segments[i].address = read64(f);
        segments[i].size = read64(f);
        segments[i].flags = read64(f);
        if (segments[i].address > size || segments[i].size > size - segments[i].address ||
            segments[i].address % page_size || segments[i].flags > 7) fail("bad segment");
    }
    if (fread(names, 128, import_count, f) != import_count) fail("truncated import names");
    for (uint64_t i = 0; i < import_count; ++i)
        if (!memchr(names[i], 0, 128)) fail("unterminated import name");
    for (uint64_t i = 0; i < nr; ++i) {
        relocs[i].target = read64(f);
        relocs[i].kind = read64(f);
        relocs[i].value = read64(f);
        relocs[i].addend = read64(f);
        if (!mapped(segments, ns, relocs[i].target, 8) || relocs[i].kind > 2 ||
            (relocs[i].kind && relocs[i].value >= import_count) ||
            (relocs[i].kind != 2 && relocs[i].addend) || relocs[i].addend >= page_size) fail("bad relocation");
    }
    for (uint64_t m=0;m<module_count;++m)
        if (!mapped(segments,ns,modules[m].init,1) || (modules[m].tls_module && !mapped(segments,ns,modules[m].tls_address,modules[m].tls_memsz)) ||
            (modules[m].procparam && !mapped(segments,ns,modules[m].procparam,0x18)))
            fail("unmapped module metadata");
    if (module_count && !mapped(segments,ns,procparam,64)) fail("unmapped procparam");
    for (uint64_t i=0;i<import_count;++i) {
        if (!bindings[i]) continue;
        if (!mapped(segments,ns,bindings[i],1)) fail("unmapped native export");
        if (binding_kinds[i]==1) {
            int executable=0;
            for (uint64_t s=0;s<ns;++s)
                if ((segments[s].flags&1) && bindings[i]>=segments[s].address &&
                    bindings[i]-segments[s].address<segments[s].size) executable=1;
            if (!executable) fail("native function is not executable");
        }
    }
    isa_table = isa_sets && isa_file ? isa_table_load(isa_file) : NULL;
    if (!isa_table && cpu_missing_sets) { printf("STOP: this CPU lacks %s and no rewrite table matches the image\n", missing); return 20; }
    /* The stubs follow the image in the same low mapping (rel32 jmps reach them). */
    size_t stub_area = isa_table ? round_page(isa_stub_area_size(isa_table)) : 0;
    /* Six more pages after them: the Display Settings page's hook stubs (runtime_display_page.c),
     * the field walk's stub (runtime_field_walk.c), the DLC bonus row's (runtime_dlc_bonus.c), the
     * Xbox pad's (runtime_xbox_pad.c), the logo and opening skip's (runtime_skip_opening.c) and the exit
     * prompt's (runtime_exit_prompt.c). On Windows the red-zone trampolines follow (redzone_protect). */
#ifdef _WIN32
    size_t redzone_area = REDZONE_TRAMPOLINES;
#else
    size_t redzone_area = 0;
#endif
    image = allocate(round_page(size) + stub_area + 6 * page_size + redzone_area);
    printf("Runtime: guest image at %p (guest offsets are relative to it)\n", (void *)image);
    unsigned char *stubs = image + round_page(size);
    if (fread(image, 1, size, f) != size || fgetc(f) != EOF) fail("incorrect memory image size");
    fclose(f);
    if (isa_table && isa_table_matches(isa_table, image, size)) {
        printf("STOP: this CPU lacks %s and no rewrite table matches the image\n", missing);
        return 20;
    }
    if (!cpu_only) {
        char title[128]="Theatrhythm", serial[16]="UNKNOWN", sfo[4096];
        uint32_t attributes=0;
        snprintf(sfo,sizeof(sfo),"%s/sce_sys/param.sfo",app0 ? app0 : ".");
        sfo_value(sfo,"TITLE",title,sizeof(title),NULL);
        sfo_value(sfo,"TITLE_ID",serial,sizeof(serial),NULL);
        sfo_value(sfo,"ATTRIBUTE",NULL,0,&attributes);
        runtime_kernel_set_psf_attributes(attributes);
        uint64_t sdk=0;
        if (procparam) memcpy(&sdk,image+procparam+16,8); /* procparam: size, magic, count, sdk_version */
        char splash[4096];
        snprintf(splash,sizeof(splash),"%s/sce_sys/pic1.png",app0 ? app0 : ".");
        TheatrhythmGpuConfig gpu={title,serial,user_dir ? user_dir : "user",(uint32_t)sdk,attributes,1920,1080,
                                  app0 ? splash : NULL};
        gpu_enabled=1; /* page tracking starts while the rasterizer registers guest memory */
        if (theatrhythm_gpu_init(&gpu)) fail("GPU initialization failed");
        printf("GPU: window and Vulkan presenter ready; SDK 0x%08x, %u HLE symbols\n",(unsigned)sdk,theatrhythm_gpu_symbol_count());
        start_hang_watch();
        if (app0) configure_button_icons(argv[1]); /* settings are read by theatrhythm_gpu_init */
        if (app0) monitor_icon = mount_config_icon(argv[1]);
        if (theatrhythm_render_4k()) {
            runtime_kernel_set_neo_mode(1);
            if (app0) mount_boot_config_4k(argv[1],app0);
        }
    }
    unsigned char *traps = allocate(round_page((import_count + 1) * 32));
    unsigned char *data_traps = allocate((import_count + 1) * page_size);
    protect(data_traps, (import_count + 1) * page_size, 0);
    for (uint64_t i = 0; i < import_count; ++i) {
        unsigned char *t = traps + i * 32;
        /* SysV: mov edi, index; movabs rax, handler; jmp rax. No fake return values. */
        uint32_t index = (uint32_t)i;
        uintptr_t handler = (uintptr_t)unresolved;
        t[0] = 0x48; t[1] = 0x89; t[2] = 0xfe; /* mov rsi,rdi: preserve arg0 */
        t[3] = 0xbf; memcpy(t + 4, &index, 4);
        t[8] = 0x48; t[9] = 0xb8; memcpy(t + 10, &handler, 8);
        t[18] = 0xff; t[19] = 0xe0;
    }
    for (uint64_t i = 0; i < nr; ++i) {
        uintptr_t value = relocs[i].kind == 2 ? (uintptr_t)(data_traps + page_size * relocs[i].value + relocs[i].addend)
                        : relocs[i].kind == 1 ? (uintptr_t)(traps + 32 * relocs[i].value)
                        : (uintptr_t)image + relocs[i].value;
        if (relocs[i].kind) {
            uintptr_t resolved = runtime_resolve(names[relocs[i].value], relocs[i].kind == 2);
            if (resolved) value = resolved + relocs[i].addend;
            else if (native_libc && bindings[relocs[i].value]) {
                uint64_t address=bindings[relocs[i].value];
                if (binding_kinds[relocs[i].value]!=relocs[i].kind || !mapped(segments,ns,address,relocs[i].addend+1)) fail("native export kind/range mismatch");
                value=(uintptr_t)image+address+relocs[i].addend;
            }
        }
        memcpy(image + relocs[i].target, &value, 8);
    }
    if (check_imports) {
        /* Lists every import the game would stop at, without running it (needs the GPU
         * library: without --cpu-only its symbols are registered above). */
        unsigned char *checked=calloc(import_count ? import_count : 1,1);
        if (!checked) fail("allocation failed");
        uint64_t missing=0;
        for (uint64_t i=0;i<nr;++i) {
            uint64_t index=relocs[i].value;
            if (!relocs[i].kind || checked[index]) continue;
            checked[index]=1;
            if (runtime_resolve(names[index],relocs[i].kind==2) || (native_libc && bindings[index])) continue;
            printf("MISSING: %s (%s)\n",names[index],runtime_import_name(names[index]));
            ++missing;
        }
        printf("Imports: %" PRIu64 " missing\n",missing);
        fflush(NULL);
        _exit(missing ? 1 : 0); /* no destructors: GPU threads are running */
    }
#ifdef _WIN32
    /* The executable segments before the loader's own code patches (redzone_protect). */
    unsigned char *code_before = NULL;
    if (!cpu_only) {
        code_before = malloc(round_page(size));
        if (!code_before) fail("allocation failed");
        for (uint64_t i = 0; i < ns; ++i)
            if (segments[i].flags & 1) memcpy(code_before + segments[i].address, image + segments[i].address, segments[i].size);
    }
#endif
    if (!cpu_only) {
        unsigned char *hook_stub = stubs + stub_area;
        for (uint64_t m = 0; m < module_count; ++m)
            if (!strcmp(modules[m].name, "Il2CppUserAssemblies.prx"))
                runtime_game_code_open(image + modules[m].base, modules[m].size, modules[m].seg0_size, app0);
        int display_page = 0;
        for (uint64_t m = 0; m < module_count; ++m)
            if (!strcmp(modules[m].name, "Il2CppUserAssemblies.prx") &&
                !runtime_display_page_install(image, modules[m].base, hook_stub, page_size, monitor_icon)) {
                protect(hook_stub, page_size, 5);
                display_page = 1;
            }
        unsigned char *walk_stub = hook_stub + page_size;
        for (uint64_t m = 0; m < module_count; ++m)
            if (!strcmp(modules[m].name, "Il2CppUserAssemblies.prx") &&
                !runtime_field_walk_install(image, modules[m].base, walk_stub, page_size))
                protect(walk_stub, page_size, 5);
        unsigned char *bonus_stub = walk_stub + page_size;
        for (uint64_t m = 0; m < module_count; ++m)
            if (!strcmp(modules[m].name, "Il2CppUserAssemblies.prx") &&
                !runtime_dlc_bonus_install(image, modules[m].base, bonus_stub, page_size))
                protect(bonus_stub, page_size, 5);
        unsigned char *pad_stub = bonus_stub + page_size;
        for (uint64_t m = 0; m < module_count; ++m)
            if (!strcmp(modules[m].name, "Il2CppUserAssemblies.prx") &&
                !runtime_xbox_pad_install(image, modules[m].base, pad_stub, page_size))
                protect(pad_stub, page_size, 5);
        unsigned char *opening_stub = pad_stub + page_size;
        for (uint64_t m = 0; m < module_count; ++m)
            if (theatrhythm_skip_opening() && !strcmp(modules[m].name, "Il2CppUserAssemblies.prx") &&
                !runtime_skip_opening_install(image, modules[m].base, opening_stub, page_size))
                protect(opening_stub, page_size, 5);
        unsigned char *exit_stub = opening_stub + page_size;
        for (uint64_t m = 0; m < module_count; ++m)
            if (!strcmp(modules[m].name, "Il2CppUserAssemblies.prx") &&
                !runtime_exit_prompt_install(image, modules[m].base, exit_stub, page_size, display_page))
                protect(exit_stub, page_size, 5);
        runtime_game_code_close();
    }
#ifdef _WIN32
    printf("Guest TCB loads redirected to the TEB TLS slot: %" PRIu64 "\n", patch_tcb_loads(segments, ns));
#endif
    if (isa_table) {
        IsaReport r = isa_rewrite_apply(isa_table, image, stubs, isa_sets);
        if (r.unsupported_needed) {
            printf("STOP: this CPU lacks %s and %u sites use encodings the rewrite cannot emulate\n", missing, r.unsupported_needed);
            fflush(NULL);
            _exit(20); /* no destructors: GPU threads are running */
        }
        if (isa_sets == 0xffffffffu) printf("ISA: forced rewrite of all %u sites (THEATRHYTHM_ISA_REWRITE=1), %u skipped\n", r.rewritten, r.skipped);
        else printf("ISA: CPU lacks %s: %u sites rewritten (%u jmp, %u relocated, %u int3), %u skipped\n",
                    missing, r.rewritten, r.jmp, r.relocated, r.int3, r.skipped);
        protect(stubs, stub_area, 5);
    }
#ifdef _WIN32
    /* Page tracking runs with the GPU only. */
    if (code_before) {
        unsigned char *trampolines = stubs + stub_area + 6 * page_size;
        redzone_protect(image, size, trampolines, code_before, segments, ns, modules, module_count, &main_module);
        redzone_probe_setup();
        protect(trampolines, redzone_area, 5);
        free(code_before);
    }
#endif
    runtime_set_report_hook(loader_report_hook);
    protect(traps, round_page((import_count + 1) * 32), 5);
    protect(image, round_page(size), 0);
    int executable_entry = 0;
    for (uint64_t i = 0; i < ns; ++i) {
        protect(image + segments[i].address, round_page(segments[i].size), (unsigned)segments[i].flags);
        if ((segments[i].flags & 1) && entry >= segments[i].address && entry - segments[i].address < segments[i].size)
            executable_entry = 1;
    }
    if (!executable_entry) fail("entry is not executable");
    printf("Mapped %" PRIu64 " bytes, %" PRIu64 " segments; applied %" PRIu64 " relocations\n", size, ns, nr);
    if (native_libc) {
        for (uint64_t m=0;m<module_count;++m) {
            int init_executable=0;
            for (uint64_t i=0;i<ns;++i)
                if ((segments[i].flags&1) && modules[m].init>=segments[i].address && modules[m].init-segments[i].address<segments[i].size) init_executable=1;
            if (!init_executable) fail("module init is not executable");
            if (modules[m].tls_module)
                runtime_set_module_tls(modules[m].tls_module,image+modules[m].tls_address,modules[m].tls_filesz,modules[m].tls_memsz);
        }
        runtime_set_main_tls(image+main_tls[0],main_tls[1],main_tls[2],main_tls[3]);
        runtime_thread_attach_main();
        runtime_orbis_fpu(); /* the PS4's floating-point state from the first guest code on */
        runtime_set_procparam(image+procparam);
        runtime_module_register(image,modules,module_count);
        runtime_module_set_main(&main_module);
        runtime_throw_trace_install();
        /* Dependencies start in link order (libc first), as the PS4 dynamic linker does. Deferred
         * modules start when the game loads them (runtime_module.c). */
        for (uint64_t m=0;m<module_count;++m) {
            if (modules[m].flags & RUNTIME_MODULE_DEFERRED) continue;
            printf("Starting linked module %" PRIu64 " at image offset 0x%" PRIx64 "; native bindings=%" PRIu64 "\n",m,modules[m].init,nb);
            typedef int (ABI *ModuleInit)(uint64_t,void *,void *);
            int result=((ModuleInit)(image+modules[m].init))(0,NULL,NULL);
            printf("Module %" PRIu64 " initializer returned %d\n",m,result);
            if (result) fail("module initializer failed");
            modules[m].started=1;
        }
    }
    printf("Entering original x86-64 code at guest offset 0x%" PRIx64 "\n", entry);
    entered_game=1;
    struct { uint64_t argc; const char *argv[2]; } params = {1, {"/app0/eboot.bin", NULL}};
#ifdef _WIN32
    runtime_enter_guest(image + entry, &params, (void *)guest_exit);
#else
    /* The guest main thread runs on a stack below 1 TiB like PS4 stacks; 16 MiB, as on Windows. */
    enum { MAIN_STACK=16*1024*1024 };
    unsigned char *stack=runtime_low_map(MAIN_STACK,PROT_READ|PROT_WRITE);
    if (!stack) fail("cannot allocate guest main stack");
    runtime_thread_set_stack(stack,MAIN_STACK);
    enter_on_stack(image+entry,&params,(void *)guest_exit,stack+MAIN_STACK-64);
#endif
    fail("entry unexpectedly returned");
}
