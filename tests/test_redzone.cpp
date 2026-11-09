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

// More table shapes. Each keeps rsi at -0x40(%rsp), stores to the tracked page in rdi in a block
// reached only through its table (the tail call: before a pointer jump) and returns rsi.
// The index copied after the bounds check (Il2Cpp: cmp al,N; ja; movzx).
static const unsigned char copied_index[] = {
    0x48, 0x89, 0x74, 0x24, 0xc0,             // mov %rsi,-0x40(%rsp)
    0x4c, 0x8d, 0x15, 0x24, 0x00, 0x00, 0x00, // lea table(%rip),%r10
    0x31, 0xc0,                               // xor %eax,%eax
    0x3c, 0x01,                               // cmp $1,%al
    0x77, 0x18,                               // ja out
    0x0f, 0xb6, 0xc8,                         // movzbl %al,%ecx
    0x49, 0x63, 0x0c, 0x8a,                   // movslq (%r10,%rcx,4),%rcx
    0x4c, 0x01, 0xd1,                         // add %r10,%rcx
    0xff, 0xe1,                               // jmp *%rcx
    0xc7, 0x07, 0x01, 0x00, 0x00, 0x00,       // case: movl $1,(%rdi)
    0x48, 0x8b, 0x44, 0x24, 0xc0,             // mov -0x40(%rsp),%rax
    0xc3,                                     // ret
    0x31, 0xc0, 0xc3,                         // out: xor %eax,%eax; ret
    0xcc, 0xcc, 0xcc,
    0xee, 0xff, 0xff, 0xff, 0xee, 0xff, 0xff, 0xff, // table: case-table (twice)
};
// The index spilled to a stack slot, moves between the cmp and its branch (FMOD 0x17278f0).
static const unsigned char spilled_index[] = {
    0x48, 0x89, 0x74, 0x24, 0xc0,             // mov %rsi,-0x40(%rsp)
    0x4c, 0x8d, 0x15, 0x34, 0x00, 0x00, 0x00, // lea table(%rip),%r10
    0x45, 0x31, 0xc0,                         // xor %r8d,%r8d
    0x41, 0x83, 0xf8, 0x01,                   // cmp $1,%r8d
    0x44, 0x89, 0xc0,                         // mov %r8d,%eax
    0x48, 0x89, 0x44, 0x24, 0xe0,             // mov %rax,-0x20(%rsp)
    0x77, 0x1a,                               // ja out
    0x48, 0x8b, 0x54, 0x24, 0xe0,             // mov -0x20(%rsp),%rdx
    0x49, 0x63, 0x14, 0x92,                   // movslq (%r10,%rdx,4),%rdx
    0x4c, 0x01, 0xd2,                         // add %r10,%rdx
    0xff, 0xe2,                               // jmp *%rdx
    0xc7, 0x07, 0x01, 0x00, 0x00, 0x00,       // case: movl $1,(%rdi)
    0x48, 0x8b, 0x44, 0x24, 0xc0,             // mov -0x40(%rsp),%rax
    0xc3,                                     // ret
    0x31, 0xc0, 0xc3,                         // out: xor %eax,%eax; ret
    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
    0xeb, 0xff, 0xff, 0xff, 0xeb, 0xff, 0xff, 0xff, // table: case-table (twice)
};
// A bound from a mask, no compare (eboot 0x16a9ff0: and $0x7fff; shr $13): 0..1 here.
static const unsigned char masked_index[] = {
    0x48, 0x89, 0x74, 0x24, 0xc0,             // mov %rsi,-0x40(%rsp)
    0x4c, 0x8d, 0x15, 0x24, 0x00, 0x00, 0x00, // lea table(%rip),%r10
    0x31, 0xc0,                               // xor %eax,%eax
    0x25, 0xff, 0x7f, 0x00, 0x00,             // and $0x7fff,%eax
    0x89, 0xc1,                               // mov %eax,%ecx
    0xc1, 0xe9, 0x0e,                         // shr $14,%ecx
    0x49, 0x63, 0x0c, 0x8a,                   // movslq (%r10,%rcx,4),%rcx
    0x4c, 0x01, 0xd1,                         // add %r10,%rcx
    0xff, 0xe1,                               // jmp *%rcx
    0xc7, 0x07, 0x01, 0x00, 0x00, 0x00,       // case: movl $1,(%rdi)
    0x48, 0x8b, 0x44, 0x24, 0xc0,             // mov -0x40(%rsp),%rax
    0xc3,                                     // ret
    0xcc, 0xcc, 0xcc,
    0xf1, 0xff, 0xff, 0xff, 0xf1, 0xff, 0xff, 0xff, // table: case-table (twice)
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
// A tail call through a pointer (IL2CPP thunks: mov 8(%rdx),%rcx; jmp *%rcx) after a short store
// with the local live: not a table, so the store can move with its neighbours.
static const unsigned char tail_call[] = {
    0x48, 0x89, 0x74, 0x24, 0xc0, // mov %rsi,-0x40(%rsp)
    0x89, 0x37,                   // mov %esi,(%rdi)
    0x48, 0x8b, 0x44, 0x24, 0xc0, // mov -0x40(%rsp),%rax
    0x48, 0x8b, 0x4a, 0x08,       // mov 8(%rdx),%rcx
    0xff, 0xe1,                   // jmp *%rcx
};

// Built from assembly (clang), offsets from each function's start.
// The table register set in an earlier block, out of reach of the nearest-lea walk (eboot
// 0x16a9ff0's loops): only its reaching definition finds the table.
static const unsigned char earlier_block[] = {
    0x48, 0x89, 0x74, 0x24, 0xc0,              // 0x00: movq %rsi, -0x40(%rsp)
    0x4c, 0x8d, 0x15, 0x24, 0x00, 0x00, 0x00,  // 0x05: leaq 0x24(%rip), %r10
    0x31, 0xc0,                                // 0x0c: xorl %eax, %eax
    0xeb, 0x01,                                // 0x0e: jmp 0x11
    0xcc,
    0x83, 0xf8, 0x01,                          // 0x11: cmpl $0x1, %eax
    0x77, 0x15,                                // 0x14: ja 0x2b
    0x49, 0x63, 0x0c, 0x82,                    // 0x16: movslq (%r10,%rax,4), %rcx
    0x4c, 0x01, 0xd1,                          // 0x1a: addq %r10, %rcx
    0xff, 0xe1,                                // 0x1d: jmpq *%rcx
    0xc7, 0x07, 0x01, 0x00, 0x00, 0x00,        // 0x1f: movl $0x1, (%rdi)
    0x48, 0x8b, 0x44, 0x24, 0xc0,              // 0x25: movq -0x40(%rsp), %rax
    0xc3,                                      // 0x2a: retq
    0x31, 0xc0,                                // 0x2b: xorl %eax, %eax
    0xc3,                                      // 0x2d: retq
    0xcc, 0xcc,
    0xef, 0xff, 0xff, 0xff, 0xef, 0xff, 0xff, 0xff, // table
};
// Refused: the nearest lea by address (another table, in a block that returns) is not the one
// reaching the load.
static const unsigned char nearest_disagrees[] = {
    0x48, 0x89, 0x74, 0x24, 0xc0,              // 0x00: movq %rsi, -0x40(%rsp)
    0x4c, 0x8d, 0x15, 0x30, 0x00, 0x00, 0x00,  // 0x05: leaq 0x30(%rip), %r10
    0x31, 0xc0,                                // 0x0c: xorl %eax, %eax
    0x85, 0xd2,                                // 0x0e: testl %edx, %edx
    0x74, 0x0a,                                // 0x10: je 0x1c
    0x4c, 0x8d, 0x15, 0x2b, 0x00, 0x00, 0x00,  // 0x12: leaq 0x2b(%rip), %r10
    0x31, 0xc0,                                // 0x19: xorl %eax, %eax
    0xc3,                                      // 0x1b: retq
    0x83, 0xf8, 0x01,                          // 0x1c: cmpl $0x1, %eax
    0x77, 0x15,                                // 0x1f: ja 0x36
    0x49, 0x63, 0x0c, 0x82,                    // 0x21: movslq (%r10,%rax,4), %rcx
    0x4c, 0x01, 0xd1,                          // 0x25: addq %r10, %rcx
    0xff, 0xe1,                                // 0x28: jmpq *%rcx
    0xc7, 0x07, 0x01, 0x00, 0x00, 0x00,        // 0x2a: movl $0x1, (%rdi)
    0x48, 0x8b, 0x44, 0x24, 0xc0,              // 0x30: movq -0x40(%rsp), %rax
    0xc3,                                      // 0x35: retq
    0x31, 0xc0,                                // 0x36: xorl %eax, %eax
    0xc3,                                      // 0x38: retq
    0xcc, 0xcc, 0xcc,
    0xee, 0xff, 0xff, 0xff, 0xee, 0xff, 0xff, 0xff, 0xf2, 0xff, 0xff, 0xff, 0xf2, 0xff, 0xff, 0xff, // table
};
// Refused: two tables reach the load.
static const unsigned char two_definitions[] = {
    0x48, 0x89, 0x74, 0x24, 0xc0,              // 0x00: movq %rsi, -0x40(%rsp)
    0x31, 0xc0,                                // 0x05: xorl %eax, %eax
    0x85, 0xd2,                                // 0x07: testl %edx, %edx
    0x74, 0x09,                                // 0x09: je 0x14
    0x4c, 0x8d, 0x15, 0x2a, 0x00, 0x00, 0x00,  // 0x0b: leaq 0x2a(%rip), %r10
    0xeb, 0x0a,                                // 0x12: jmp 0x1e
    0x4c, 0x8d, 0x15, 0x29, 0x00, 0x00, 0x00,  // 0x14: leaq 0x29(%rip), %r10
    0xeb, 0x01,                                // 0x1b: jmp 0x1e
    0xcc,
    0x83, 0xf8, 0x01,                          // 0x1e: cmpl $0x1, %eax
    0x77, 0x15,                                // 0x21: ja 0x38
    0x49, 0x63, 0x0c, 0x82,                    // 0x23: movslq (%r10,%rax,4), %rcx
    0x4c, 0x01, 0xd1,                          // 0x27: addq %r10, %rcx
    0xff, 0xe1,                                // 0x2a: jmpq *%rcx
    0xc7, 0x07, 0x01, 0x00, 0x00, 0x00,        // 0x2c: movl $0x1, (%rdi)
    0x48, 0x8b, 0x44, 0x24, 0xc0,              // 0x32: movq -0x40(%rsp), %rax
    0xc3,                                      // 0x37: retq
    0x31, 0xc0,                                // 0x38: xorl %eax, %eax
    0xc3,                                      // 0x3a: retq
    0xcc,
    0xf0, 0xff, 0xff, 0xff, 0xf0, 0xff, 0xff, 0xff, 0xf4, 0xff, 0xff, 0xff, 0xf4, 0xff, 0xff, 0xff, // table
};
// Refused: the compare checks al, the index is all of eax.
static const unsigned char narrow_compare[] = {
    0x48, 0x89, 0x74, 0x24, 0xc0,              // 0x00: movq %rsi, -0x40(%rsp)
    0x4c, 0x8d, 0x15, 0x20, 0x00, 0x00, 0x00,  // 0x05: leaq 0x20(%rip), %r10
    0x89, 0xd0,                                // 0x0c: movl %edx, %eax
    0x3c, 0x01,                                // 0x0e: cmpb $0x1, %al
    0x77, 0x17,                                // 0x10: ja 0x29
    0x89, 0xc1,                                // 0x12: movl %eax, %ecx
    0x49, 0x63, 0x0c, 0x8a,                    // 0x14: movslq (%r10,%rcx,4), %rcx
    0x4c, 0x01, 0xd1,                          // 0x18: addq %r10, %rcx
    0xff, 0xe1,                                // 0x1b: jmpq *%rcx
    0xc7, 0x07, 0x01, 0x00, 0x00, 0x00,        // 0x1d: movl $0x1, (%rdi)
    0x48, 0x8b, 0x44, 0x24, 0xc0,              // 0x23: movq -0x40(%rsp), %rax
    0xc3,                                      // 0x28: retq
    0x31, 0xc0,                                // 0x29: xorl %eax, %eax
    0xc3,                                      // 0x2b: retq
    0xf1, 0xff, 0xff, 0xff, 0xf1, 0xff, 0xff, 0xff, // table
};
// Refused: a way into the path between the bounds check and the jump.
static const unsigned char join_in_guard[] = {
    0x48, 0x89, 0x74, 0x24, 0xc0,              // 0x00: movq %rsi, -0x40(%rsp)
    0x4c, 0x8d, 0x15, 0x24, 0x00, 0x00, 0x00,  // 0x05: leaq 0x24(%rip), %r10
    0x89, 0xd0,                                // 0x0c: movl %edx, %eax
    0x85, 0xc9,                                // 0x0e: testl %ecx, %ecx
    0x75, 0x05,                                // 0x10: jne 0x17
    0x83, 0xf8, 0x01,                          // 0x12: cmpl $0x1, %eax
    0x77, 0x15,                                // 0x15: ja 0x2c
    0x49, 0x63, 0x0c, 0x82,                    // 0x17: movslq (%r10,%rax,4), %rcx
    0x4c, 0x01, 0xd1,                          // 0x1b: addq %r10, %rcx
    0xff, 0xe1,                                // 0x1e: jmpq *%rcx
    0xc7, 0x07, 0x01, 0x00, 0x00, 0x00,        // 0x20: movl $0x1, (%rdi)
    0x48, 0x8b, 0x44, 0x24, 0xc0,              // 0x26: movq -0x40(%rsp), %rax
    0xc3,                                      // 0x2b: retq
    0x31, 0xc0,                                // 0x2c: xorl %eax, %eax
    0xc3,                                      // 0x2e: retq
    0xcc,
    0xf0, 0xff, 0xff, 0xff, 0xf0, 0xff, 0xff, 0xff, // table
};
// Refused as a tail call: a way in between the pointer load and the jump (rcx unknown there).
static const unsigned char joined_tail_call[] = {
    0x48, 0x89, 0x74, 0x24, 0xc0,              // 0x00: movq %rsi, -0x40(%rsp)
    0x85, 0xc9,                                // 0x05: testl %ecx, %ecx
    0x75, 0x04,                                // 0x07: jne 0x0d
    0x48, 0x8b, 0x4a, 0x08,                    // 0x09: movq 0x8(%rdx), %rcx
    0xff, 0xe1,                                // 0x0d: jmpq *%rcx
};
// Refused: a table of absolute pointers (indexed), not a tail call.
static const unsigned char pointer_table[] = {
    0x48, 0x89, 0x74, 0x24, 0xc0,              // 0x00: movq %rsi, -0x40(%rsp)
    0x4c, 0x8d, 0x15, 0x09, 0x00, 0x00, 0x00,  // 0x05: leaq 0x9(%rip), %r10
    0x31, 0xc0,                                // 0x0c: xorl %eax, %eax
    0x49, 0x8b, 0x0c, 0xc2,                    // 0x0e: movq (%r10,%rax,8), %rcx
    0xff, 0xe1,                                // 0x12: jmpq *%rcx
    0xcc,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // table
};
// Refused: table B (found only behind table A) jumps into A's guarded path, so A's bound does
// not hold; known only once all edges are.
static const unsigned char late_join[] = {
    0x48, 0x89, 0x74, 0x24, 0xc0,              // 0x00: movq %rsi, -0x40(%rsp)
    0x4c, 0x8d, 0x15, 0x2c, 0x00, 0x00, 0x00,  // 0x05: leaq 0x2c(%rip), %r10
    0x31, 0xc0,                                // 0x0c: xorl %eax, %eax
    0x83, 0xf8, 0x01,                          // 0x0e: cmpl $0x1, %eax
    0x77, 0x21,                                // 0x11: ja 0x34
    0x49, 0x63, 0x0c, 0x82,                    // 0x13: movslq (%r10,%rax,4), %rcx
    0x4c, 0x01, 0xd1,                          // 0x17: addq %r10, %rcx
    0xff, 0xe1,                                // 0x1a: jmpq *%rcx
    0x4c, 0x8d, 0x1d, 0x1d, 0x00, 0x00, 0x00,  // 0x1c: leaq 0x1d(%rip), %r11
    0x31, 0xd2,                                // 0x23: xorl %edx, %edx
    0x83, 0xfa, 0x01,                          // 0x25: cmpl $0x1, %edx
    0x77, 0x0a,                                // 0x28: ja 0x34
    0x4d, 0x63, 0x04, 0x93,                    // 0x2a: movslq (%r11,%rdx,4), %r8
    0x4d, 0x01, 0xd8,                          // 0x2e: addq %r11, %r8
    0x41, 0xff, 0xe0,                          // 0x31: jmpq *%r8
    0x31, 0xc0,                                // 0x34: xorl %eax, %eax
    0xc3,                                      // 0x36: retq
    0xcc,
    0xe4, 0xff, 0xff, 0xff, 0xe4, 0xff, 0xff, 0xff, 0xd3, 0xff, 0xff, 0xff, 0xf4, 0xff, 0xff, 0xff, // table
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

    const unsigned long long starts[] = {0x100, 0x200, 0x300, 0x400, 0x480, 0x500, 0x580, 0x600, 0x680};
    struct { const unsigned char* code; size_t size; int start; const char* what; } const shapes[] = {
        {copied_index, sizeof(copied_index), 3, "index copied after the check"},
        {spilled_index, sizeof(spilled_index), 4, "index spilled to a stack slot"},
        {masked_index, sizeof(masked_index), 5, "index bounded by a mask"},
        {table_loop, sizeof(table_loop), 6, "local live through the table's loop"},
        {earlier_block, sizeof(earlier_block), 8, "table register set in an earlier block"},
    };
    const int shape_count = sizeof(shapes) / sizeof(shapes[0]);
    emit(image + starts[0], long_store, sizeof(long_store));
    emit(image + starts[1], short_store, sizeof(short_store));
    std::memcpy(image + starts[2], dispatch, sizeof(dispatch));
    for (int i = 0; i < shape_count; ++i) std::memcpy(image + starts[shapes[i].start], shapes[i].code, shapes[i].size);
    std::memcpy(image + starts[7], tail_call, sizeof(tail_call));
    image[0x900] = 0xc3; // the tail call's target: ret
    TheatrhythmRedZoneSegment segment{0x100, 0x700, starts, 9, nullptr, 0};
    TheatrhythmRedZoneResult result;
    int status = theatrhythm_redzone_patch(image, 0x800, trampolines, 1 << 20, &segment, 1, &result);
    std::printf("patch: status %d, %llu of %llu accesses moved, %llu not relocatable, %llu trampoline bytes\n",
                status, (unsigned long long)result.patched_memory_instruction_count,
                (unsigned long long)result.memory_instruction_count,
                (unsigned long long)result.unrelocatable_memory_instruction_count,
                (unsigned long long)result.trampoline_bytes);
    check(status == 0 && result.patched_memory_instruction_count == result.memory_instruction_count &&
              result.memory_instruction_count >= 9, "every access moved to trampolines");
    check(result.indirect_red_zone_function_count == 0, "every jump table resolved, the tail call not taken for one");
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
    for (int i = 0; i < shape_count; ++i) {
        faults = 0;
        *(unsigned*)tracked = 0;
        got = call(image + starts[shapes[i].start], a, b, c);
        char what[128];
        std::snprintf(what, sizeof(what), "%s: one fault, local intact", shapes[i].what);
        check(faults == 1 && got == a && *(unsigned*)tracked == 1u, what);
    }
    const unsigned long long thunk[2] = {0, (unsigned long long)(uintptr_t)(image + 0x900)};
    faults = 0;
    got = call(image + starts[7], a, (unsigned long long)(uintptr_t)thunk, c);
    check(faults == 1 && got == a && *(unsigned*)tracked == (unsigned)a, "store before a tail call: one fault, local intact");

    // Jumps the pass must not follow: each function alone, patched (never run) with trampolines of
    // its own, must count as one with an unresolved jump.
    struct { const unsigned char* code; size_t size; const char* what; } const refused[] = {
        {nearest_disagrees, sizeof(nearest_disagrees), "nearest lea is another table"},
        {two_definitions, sizeof(two_definitions), "two tables reach the load"},
        {narrow_compare, sizeof(narrow_compare), "compare narrower than the index"},
        {join_in_guard, sizeof(join_in_guard), "way into the guarded path"},
        {joined_tail_call, sizeof(joined_tail_call), "way in between pointer load and jump"},
        {pointer_table, sizeof(pointer_table), "table of absolute pointers"},
        {late_join, sizeof(late_join), "later table jumps into the guarded path"},
    };
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        const unsigned long long start = 0x10000 + i * 0x100;
        std::memcpy(image + start, refused[i].code, refused[i].size);
        TheatrhythmRedZoneSegment alone{start, 0x100, &start, 1, nullptr, 0};
        status = theatrhythm_redzone_patch(image, 0x20000, image + (3 << 19), 1 << 19, &alone, 1, &result);
        char what[128];
        std::snprintf(what, sizeof(what), "refused: %s", refused[i].what);
        check(status == 0 && result.red_zone_function_count == 1 &&
                  result.indirect_red_zone_function_count == 1, what);
    }
    return failures != 0;
}
#else
int main() {
    std::puts("skipped: Linux delivers signals below the red zone");
    return 0;
}
#endif
