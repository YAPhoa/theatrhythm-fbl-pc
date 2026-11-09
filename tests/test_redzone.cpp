// SPDX-License-Identifier: GPL-2.0-or-later
// Red-zone protection (#12): a SysV leaf function keeps locals below rsp and stores into a
// write-protected page. Windows writes the exception frame below rsp while the fault is handled;
// after theatrhythm_redzone_patch the store runs from a trampoline with rsp 128 bytes lower, so the
// locals survive. The unpatched copy is run as a control and only reported (whether its locals get
// overwritten depends on which XSAVE components are in use on this CPU).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#include "../gpu/theatrhythm_gpu.h"

static unsigned char* tracked; // the "GPU-tracked" page
static volatile LONG faults;

static LONG CALLBACK unprotect(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* r = info->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 ||
        (unsigned char*)r->ExceptionInformation[1] - tracked >= 4096)
        return EXCEPTION_CONTINUE_SEARCH;
    DWORD old;
    VirtualProtect(tracked, 4096, PAGE_READWRITE, &old);
    InterlockedIncrement(&faults);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// rdi = tracked page, rsi/rdx/rcx = values kept at -0x40/-0x60/-0x80(%rsp); returns their sum.
static const unsigned char locals[] = {
    0x48, 0x89, 0x74, 0x24, 0xc0, // mov %rsi,-0x40(%rsp)
    0x48, 0x89, 0x54, 0x24, 0xa0, // mov %rdx,-0x60(%rsp)
    0x48, 0x89, 0x4c, 0x24, 0x80, // mov %rcx,-0x80(%rsp)
};
static const unsigned char sum[] = {
    0x48, 0x8b, 0x44, 0x24, 0xc0, // mov -0x40(%rsp),%rax
    0x48, 0x03, 0x44, 0x24, 0xa0, // add -0x60(%rsp),%rax
    0x48, 0x03, 0x44, 0x24, 0x80, // add -0x80(%rsp),%rax
    0xc3,                         // ret
};
static const unsigned char long_store[] = {0xc7, 0x07, 0x01, 0x00, 0x00, 0x00}; // movl $1,(%rdi)
static const unsigned char short_store[] = {0x89, 0x37};                         // mov %esi,(%rdi)

// A leaf that dispatches through a jump table, the compiler having scheduled an unrelated move
// between the table load and the add (eboot 0xd34790): its case block, reached only through the
// table, keeps rsi at -0x40(%rsp) across a store to the tracked page in rdi; returns it.
static const unsigned char dispatch[] = {
    0x48, 0x89, 0x74, 0x24, 0xc0,             // mov %rsi,-0x40(%rsp)
    0x4c, 0x8d, 0x15, 0x24, 0x00, 0x00, 0x00, // lea table(%rip),%r10
    0x31, 0xc0,                               // xor %eax,%eax
    0x83, 0xf8, 0x01,                         // cmp $1,%eax
    0x77, 0x19,                               // ja out
    0x49, 0x63, 0x0c, 0x82,                   // movslq (%r10,%rax,4),%rcx
    0x49, 0x89, 0xfb,                         // mov %rdi,%r11
    0x4c, 0x01, 0xd1,                         // add %r10,%rcx
    0xff, 0xe1,                               // jmp *%rcx
    0x41, 0xc7, 0x03, 0x01, 0x00, 0x00, 0x00, // case: movl $1,(%r11)
    0x48, 0x8b, 0x44, 0x24, 0xc0,             // mov -0x40(%rsp),%rax
    0xc3,                                     // ret
    0x31, 0xc0, 0xc3,                         // out: xor %eax,%eax; ret
    0xcc,
    0xef, 0xff, 0xff, 0xff, 0xef, 0xff, 0xff, 0xff, // table: case-table (twice)
};

// A loop through its table (eboot 0xd34790): the store comes after the case's last use of the
// local, which is read again on the next pass, so the local is live there only through the
// table's edge back into the case. r9d counts the passes.
static const unsigned char table_loop[] = {
    0x48, 0x89, 0x74, 0x24, 0xc0,             // mov %rsi,-0x40(%rsp)
    0x4c, 0x8d, 0x15, 0x34, 0x00, 0x00, 0x00, // lea table(%rip),%r10
    0x45, 0x31, 0xc9,                         // xor %r9d,%r9d
    0x31, 0xc0,                               // head: xor %eax,%eax
    0x83, 0xf8, 0x01,                         // cmp $1,%eax
    0x77, 0x23,                               // ja out
    0x49, 0x63, 0x0c, 0x82,                   // movslq (%r10,%rax,4),%rcx
    0x49, 0x89, 0xfb,                         // mov %rdi,%r11
    0x4c, 0x01, 0xd1,                         // add %r10,%rcx
    0xff, 0xe1,                               // jmp *%rcx
    0x45, 0x85, 0xc9,                         // case: test %r9d,%r9d
    0x75, 0x0c,                               // jnz second
    0x41, 0xc7, 0x03, 0x01, 0x00, 0x00, 0x00, // movl $1,(%r11)
    0x41, 0xff, 0xc1,                         // inc %r9d
    0xeb, 0xdc,                               // jmp head
    0x48, 0x8b, 0x44, 0x24, 0xc0,             // second: mov -0x40(%rsp),%rax
    0xc3,                                     // ret
    0x31, 0xc0, 0xc3,                         // out: xor %eax,%eax; ret
    0xcc, 0xcc, 0xcc, 0xcc,
    0xe2, 0xff, 0xff, 0xff, 0xe2, 0xff, 0xff, 0xff, // table: case-table (twice)
};
static size_t emit(unsigned char* at, const unsigned char* store, size_t store_size) {
    size_t n = 0;
    std::memcpy(at + n, locals, sizeof(locals)), n += sizeof(locals);
    std::memcpy(at + n, store, store_size), n += store_size;
    std::memcpy(at + n, sum, sizeof(sum)), n += sizeof(sum);
    std::memset(at + n, 0xcc, 16);
    return n;
}

// Calls fn with the upper ymm halves (and zmm16-31 where present) in use, as guest code runs.
static unsigned long long call(unsigned char* fn, unsigned long long a, unsigned long long b,
                               unsigned long long c) {
    unsigned long long result;
    DWORD old;
    VirtualProtect(tracked, 4096, PAGE_READONLY, &old);
    if (__builtin_cpu_supports("avx512f"))
        __asm__ volatile(".byte 0x62,0x03,0x05,0x40,0x25,0xff,0xff" ::: "memory"); // vpternlogd $0xff,zmm31,zmm31,zmm31
    __asm__ volatile("vpcmpeqd %%ymm15,%%ymm15,%%ymm15\n\tcall *%[fn]"
                     : "=a"(result), "+S"(a), "+d"(b), "+c"(c)
                     : [fn] "r"(fn), "D"(tracked)
                     : "r8", "r9", "r10", "r11", "xmm15", "memory", "cc");
    return result;
}

static int failures;
static void check(int ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++failures;
}

int main() {
    const unsigned long long a = 0x1111111111111111, b = 0x2222222222222222, c = 0x4444444444444444;
    unsigned char* image = (unsigned char*)VirtualAlloc(nullptr, 2 << 20, MEM_RESERVE | MEM_COMMIT,
                                                        PAGE_EXECUTE_READWRITE);
    tracked = (unsigned char*)VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!image || !tracked) return 1;
    AddVectoredExceptionHandler(1, unprotect);
    std::memset(image, 0xcc, 1 << 20);
    unsigned char* trampolines = image + (1 << 20);

    // Control: an unpatched copy.
    emit(image + 0x800, long_store, sizeof(long_store));
    faults = 0;
    unsigned long long control = call(image + 0x800, a, b, c);
    std::printf("control (unpatched): faults=%ld, locals %s\n", faults,
                control == a + b + c ? "intact" : "overwritten by the exception frame");

    const unsigned long long starts[] = {0x100, 0x200, 0x300, 0x400};
    emit(image + starts[0], long_store, sizeof(long_store));
    emit(image + starts[1], short_store, sizeof(short_store));
    std::memcpy(image + starts[2], dispatch, sizeof(dispatch));
    std::memcpy(image + starts[3], table_loop, sizeof(table_loop));
    TheatrhythmRedZoneSegment segment{0x100, 0x380, starts, 4, nullptr, 0};
    TheatrhythmRedZoneResult result;
    int status = theatrhythm_redzone_patch(image, 0x800, trampolines, 1 << 20, &segment, 1, &result);
    std::printf("patch: status %d, %llu of %llu accesses moved, %llu not relocatable, %llu trampoline bytes\n",
                status, (unsigned long long)result.patched_memory_instruction_count,
                (unsigned long long)result.memory_instruction_count,
                (unsigned long long)result.unrelocatable_memory_instruction_count,
                (unsigned long long)result.trampoline_bytes);
    check(status == 0 && result.patched_memory_instruction_count == result.memory_instruction_count &&
              result.memory_instruction_count >= 3, "every access moved to trampolines");
    check(result.indirect_red_zone_function_count == 0, "jump table resolved past the scheduled move");
    check(std::memcmp(image + starts[0], locals, sizeof(locals)) == 0, "red-zone spills left in place");

    for (int i = 0; i < 2; ++i) {
        faults = 0;
        unsigned long long got = call(image + starts[i], a, b, c);
        char what[96];
        std::snprintf(what, sizeof(what), "%s store: one fault, locals intact", i ? "short" : "long");
        check(faults == 1 && got == a + b + c, what);
        check(*(unsigned*)tracked == (i ? (unsigned)a : 1u), "store reached the tracked page");
    }
    faults = 0;
    *(unsigned*)tracked = 0;
    unsigned long long got = call(image + starts[2], a, b, c);
    check(faults == 1 && got == a, "jump-table case store: one fault, local intact");
    check(*(unsigned*)tracked == 1u, "store reached the tracked page");
    faults = 0;
    *(unsigned*)tracked = 0;
    got = call(image + starts[3], a, b, c);
    check(faults == 1 && got == a && *(unsigned*)tracked == 1u, "local live through the table's loop: one fault, local intact");
    return failures != 0;
}
#else
int main() {
    std::puts("skipped: Linux delivers signals below the red zone");
    return 0;
}
#endif
