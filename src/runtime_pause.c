/* F4 pauses the game anywhere, and F4 again goes on (theatrhythm_overlay.cpp takes the key). Nothing
 * of the game's is called or changed: its main thread waits at the start of
 * ProcessLifecyclePS4::Update, between two scripts of a frame, as in a long hitch. While it waits,
 * the guest's monotonic clocks stand still (runtime_kernel.c, so Unity's time, FMOD's and every timer
 * go on afterwards as if no time had passed), sceAudioOutOutput holds the sound (FMOD's mixer, and
 * with it the song, stops where it is) and the present thread shows the last frame under a
 * "Paused" note without vblank or flip events (DebugState.IsGuestThreadsPaused). Other threads
 * (loading, a save being written) finish what they are doing and then wait for the main thread as
 * they would in any long frame. The hook site is found by name and checked by its bytes
 * (runtime_game_code.c); in a game version without it, F4 shows a notice instead. */
#include "runtime.h"
#include "gpu/theatrhythm_gpu.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include "compat_win.h"
#endif

/* The registers a hook sees (the stub's frame): the six argument registers, and what the game
 * function returns when the hook returns 1. */
typedef struct { uint64_t rdi, rsi, rdx, rcx, r8, r9, rax, rdx_out; } HookRegs;
typedef int (ABI *HookFn)(HookRegs *regs);

static const GameSite lifecycle_site={"Gummi.Device.PS4.ProcessLifecyclePS4::Update", 0, 0, 0,
    {"55 48 89 e5 41 56 53 | 48 8b 0d ?? ?? ?? ?? 8a 5f 38 49 89 fe 48 85 c9 75 1f"}};

static int paused; /* the main thread waits in lifecycle_update */

static void sleep_ms(unsigned ms) {
#ifdef _WIN32
    compat_sleep_ns((uint64_t)ms*1000000u);
#else
    struct timespec t={0,(long)ms*1000000L};
    nanosleep(&t,NULL);
#endif
}

int runtime_paused(void) { return __atomic_load_n(&paused,__ATOMIC_ACQUIRE); }
void runtime_pause_wait(void) { while (runtime_paused()) sleep_ms(5); }

/* ProcessLifecyclePS4::Update(this), on the main thread every frame: while F4 holds the pause, wait
 * here with the clock held. */
static ABI int lifecycle_update(HookRegs *r) {
    (void)r;
    if (!theatrhythm_gpu_pause_requested()) return 0;
    runtime_clock_hold(1);
    __atomic_store_n(&paused,1,__ATOMIC_RELEASE);
    theatrhythm_gpu_set_paused(1);
    puts("Pause: on (F4 goes on)");
    fflush(stdout);
    while (theatrhythm_gpu_pause_requested()) sleep_ms(10);
    runtime_clock_hold(0);
    __atomic_store_n(&paused,0,__ATOMIC_RELEASE);
    theatrhythm_gpu_set_paused(0);
    puts("Pause: off");
    return 0;
}

static unsigned char *emit(unsigned char *p, const void *bytes, size_t size) { memcpy(p,bytes,size); return p+size; }
/* mov [rsp+disp], reg (store) or mov reg, [rsp+disp] (load); reg in x86 numbering. */
static unsigned char *rsp_slot(unsigned char *p, int store, int reg, uint8_t disp) {
    const unsigned char bytes[]={(unsigned char)(0x48|(reg>=8 ? 4 : 0)),store ? 0x89 : 0x8b,(unsigned char)(0x44|(reg&7)<<3),0x24,disp};
    return emit(p,bytes,sizeof bytes);
}
#define STUB_SIZE 144 /* the most emit_stub writes (16 moved bytes at most) */
/* The stub for `fn` at `p`, as the Display page's: the argument registers saved in a HookRegs on
 * the stack (the call 16-byte aligned: rsp is 8 mod 16 at the entry), fn called, then its result
 * returned, or the registers restored, the moved prologue run and the function entered after it. */
static unsigned char *emit_stub(unsigned char *p, HookFn fn, unsigned char *function, unsigned moved) {
    static const int args[6]={7,6,2,1,8,9}; /* rdi rsi rdx rcx r8 r9 */
    const int32_t frame=72;
    p=emit(p,(const unsigned char[]){0x48,0x81,0xec},3); p=emit(p,&frame,4); /* sub rsp,frame */
    for (int i=0;i<6;++i) p=rsp_slot(p,1,args[i],(uint8_t)(8*i));
    p=emit(p,(const unsigned char[]){0x48,0x89,0xe7},3);                       /* mov rdi,rsp */
    const uint64_t call=(uint64_t)(uintptr_t)fn;
    p=emit(p,(const unsigned char[]){0x48,0xb8},2); p=emit(p,&call,8);        /* mov rax,fn */
    p=emit(p,(const unsigned char[]){0xff,0xd0,0x85,0xc0},4);                 /* call rax; test eax,eax */
    unsigned char *jnz=p; p=emit(p,(const unsigned char[]){0x75,0},2);        /* jnz ret */
    for (int i=0;i<6;++i) p=rsp_slot(p,0,args[i],(uint8_t)(8*i));
    p=emit(p,(const unsigned char[]){0x48,0x81,0xc4},3); p=emit(p,&frame,4); /* add rsp,frame */
    p=emit(p,function,moved);                                                  /* the moved bytes */
    const uint64_t back=(uint64_t)(uintptr_t)(function+moved);
    p=emit(p,(const unsigned char[]){0xff,0x25,0,0,0,0},6); p=emit(p,&back,8); /* jmp [rip]: back */
    jnz[1]=(unsigned char)(p-(jnz+2));
    p=rsp_slot(p,0,0,48); p=rsp_slot(p,0,2,56);                               /* rax, rdx = results */
    p=emit(p,(const unsigned char[]){0x48,0x81,0xc4},3); p=emit(p,&frame,4); /* add rsp,frame */
    return emit(p,(const unsigned char[]){0xc3},1);                           /* ret */
}

int runtime_pause_install(unsigned char *image, uint64_t module_base, unsigned char *stubs, size_t size) {
    GameCode at;
    if (runtime_game_code_find(&lifecycle_site,1,&at,"Pause") || size<STUB_SIZE) {
        puts("Pause: off, F4 does nothing in this game version");
        return -1;
    }
    unsigned char *function=image+module_base+at.offset;
    const int64_t rel=(int64_t)(stubs-(function+5));
    if (at.moved<5 || at.moved>16 || rel!=(int32_t)rel) { puts("Pause: off (hook out of reach, runner bug)"); return -1; }
    emit_stub(stubs,lifecycle_update,function,at.moved);
    const int32_t rel32=(int32_t)rel;
    function[0]=0xe9; memcpy(function+1,&rel32,4);
    memset(function+5,0x90,at.moved-5u); /* never run: the stub jumps past them */
    theatrhythm_gpu_set_pause_available(1);
    puts("Pause: F4 pauses the game and goes on");
    return 0;
}
