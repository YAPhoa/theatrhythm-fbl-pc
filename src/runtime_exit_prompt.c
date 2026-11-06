/* "Exit the game?" in the game's own Yes/No window, with its own look and sounds. F4 asks for it
 * (theatrhythm_overlay.cpp); on the game thread, ProcessLifecyclePS4::Update (every frame) opens it
 * when WindowManager.IsOpenableSystemWindow() allows. During a song it first pauses the song as the
 * PS4's system menu does (MusicStageMainLoop.callSystemPause: no pause menu, which would take the
 * keys under the window) and opens the window once the song stands; No lets the song go on the same
 * way, after the 3-2-1 its Resume Song shows (MusicStageMainLoop::resume with countdown). The window is the Language page's "Restore default settings?"
 * (ConfigLanguageMenuEditPartsMain::openResetSettingWindow: one CommonOnlyText window with No and
 * Yes, No focused), called with the lifecycle object instead of the page; the page is only its
 * reaction callback's target. The Display page's translation hook shows this text in it
 * (runtime_exit_prompt_translation), and the callback, onExecuteResetSettingWindow, is answered here
 * for that target: Yes (ReactionType Submit) ends the game as closing its window does, once no save
 * is mounted or being written (runtime_savedata_busy, at most 10 s). Every site is found by name and
 * checked by its bytes (runtime_game_code.c); a game version without them has no prompt, and F4
 * says so. */
#include "runtime.h"
#include "gpu/theatrhythm_gpu.h"
#include <stdio.h>
#include <string.h>

/* The registers a hook sees (the stub's frame): the six argument registers, and what the game
 * function returns when the hook returns 1. */
typedef struct { uint64_t rdi, rsi, rdx, rcx, r8, r9, rax, rdx_out; } HookRegs;
typedef int (ABI *HookFn)(HookRegs *regs);

enum { H_LIFECYCLE, H_WINDOW, H_STAGE, H_REACTION, H_RESUME, C_OPEN, C_OPENABLE, C_SYSTEM_PAUSE, SITES };
static const GameSite sites[SITES]={
    {"Gummi.Device.PS4.ProcessLifecyclePS4::Update", 0, 0, 0,
     {"55 48 89 e5 41 56 53 | 48 8b 0d ?? ?? ?? ?? 8a 5f 38 49 89 fe 48 85 c9 75 1f"}},
    {"Gummi.Scene.Window.WindowMain::Update", 0, 0, 0,
     {"55 48 89 e5 53 50 | 8b 47 78 8d 48 ff 83 f9 01 77 32 31 f6 83 f8 02 48 8b 07"}},
    {"Game.Scene.MusicStage.MusicStageMainLoop::updateState", 0, 0, 0,
     {"55 48 89 e5 41 56 53 | 48 83 ec 10 80 3d ?? ?? ?? ?? 00 48 89 fb 75 1f 48 8d 3d ?? ?? ?? ??"}},
    /* up to its check of the reaction: the class is ReactionData's, then cmp dword [rbx+0x18],0
     * (ReactionType Submit); je */
    {"Game.Scene.Config.ConfigLanguageMenuEditPartsMain::onExecuteResetSettingWindow", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 53 50 80 3d ?? ?? ?? ?? 00 48 89 d3 49 89 fe 75 1f 48 8d 3d ?? ?? ?? ?? "
      "e8 ?? ?? ?? ?? 48 8d 3d ?? ?? ?? ?? e8 ?? ?? ?? ?? c6 05 ?? ?? ?? ?? 01 48 85 db 74 2d 48 8b 05 "
      "?? ?? ?? ?? 48 8b 0b 0f b6 90 2c 01 00 00 38 91 2c 01 00 00 72 14 48 8b 89 c8 00 00 00 48 39 44 "
      "d1 f8 75 06 83 7b 18 00 74 0b"}},
    /* resume(this, ?, countdown): the synchronous one; the end of a system pause passes countdown false */
    {"Game.Scene.MusicStage.MusicStageMainLoop::resume(2)", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 53 50 | 80 3d ?? ?? ?? ?? 00 41 89 d6 49 89 ff 75 13"}},
    {"Game.Scene.Config.ConfigLanguageMenuEditPartsMain::openResetSettingWindow", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 41 55 41 54 53 50 80 3d ?? ?? ?? ?? 00 49 89 fd"}},
    {"Game.WindowManager::IsOpenableSystemWindow", 0, 0, 0,
     {"55 48 89 e5 80 3d ?? ?? ?? ?? 00 75 37 48 8d 3d ?? ?? ?? ??"}},
    /* callSystemPause(this, on): requests the system pause (byte 0x11d) or its end (0x11e) */
    {"Game.Scene.MusicStage.MusicStageMainLoop::callSystemPause", 0, 0, 0,
     {"b0 01 85 f6 75 0b 80 bf 1d 01 00 00 00 74 11 31 c0 88 87 1d 01 00 00 31 c0"}},
};
static unsigned char *module;
static GameCode code_at[SITES];
#define CODE(site) ((void *)(module+code_at[site].offset))

static void *target;          /* the lifecycle object the open window's callback goes to */
static void *stage;           /* the music stage loop updating now, if any */
static void *paused_stage;    /* the one the prompt paused */
static void *counting_stage;  /* the one whose next resume counts 3-2-1 */
static volatile int asking;   /* that window is open */
static int ending;            /* Yes chosen: the game ends once no save is busy */
/* Frames (lifecycle updates), the last one a game window or a music stage updated in, the one the
 * song was paused in for the prompt (0: none), the one the window opened in and the one Yes was
 * chosen in. */
static unsigned frame, window_frame, stage_frame, paused_song_frame, open_frame, yes_frame;
/* the song stands; 10 s at 60 fps; no window update for half a second: the prompt's window is gone */
enum { PAUSE_FRAMES=6, SAVE_WAIT_FRAMES=600, GONE_FRAMES=30 };

static int32_t load32(const void *p) { int32_t v; memcpy(&v,p,4); return v; }
static const char *class_name(const void *object) { /* Il2CppClass: image, gc_desc, name */
    const char *klass; memcpy(&klass,object,8);
    const char *name; memcpy(&name,klass+0x10,8);
    return name;
}

/* WindowMain::Update(this): a game window is open (every window's scene updates through it). */
static ABI int window_update(HookRegs *r) { (void)r; window_frame=frame; return 0; }
/* MusicStageMainLoop::updateState(this): a music stage runs (a song, or its pause screen). */
static ABI int stage_update(HookRegs *r) { stage=(void *)r->rdi; stage_frame=frame; return 0; }
static void system_pause(void *loop, int on) { ((void (ABI *)(void *, int32_t, void *))CODE(C_SYSTEM_PAUSE))(loop,on,NULL); }
/* Lets the song the prompt paused go on, after the game's 3-2-1 as its pause menu's Resume Song. */
static void end_song_pause(void) {
    if (!paused_stage) return;
    counting_stage=paused_stage;
    system_pause(paused_stage,0);
    paused_stage=NULL;
}
/* MusicStageMainLoop::resume(this, ?, countdown): the end of the system pause the prompt started
 * counts down (ResumeCounter) like Resume Song instead of going on at once. */
static ABI int stage_resume(HookRegs *r) {
    if (counting_stage && (void *)r->rdi==counting_stage) {
        counting_stage=NULL;
        r->rdx=1;
        puts("Exit prompt: the song goes on after its countdown");
    }
    return 0;
}

/* Opens the window for `lifecycle`, unless another game window is open (the game's own check does
 * not count them, and a second window of the same scene throws through this hook, which ends the
 * game) or the game says no system window may open now (a scene change). `over_pause`: the song was
 * paused for it, so the pause screen may count as a window. */
static void open_prompt(void *lifecycle, int over_pause) {
    if (frame-window_frame<=2) {
        puts("Exit prompt: not now (a window is open)");
        theatrhythm_gpu_notice("Close this window first to exit with F4",3000);
        if (over_pause) end_song_pause();
        return;
    }
    if (!(((uint8_t (ABI *)(void *))CODE(C_OPENABLE))(NULL)&1)) { /* WindowManager.IsOpenableSystemWindow() */
        puts("Exit prompt: not now (the game allows no window here)");
        theatrhythm_gpu_notice("F4 can't exit here: try again in a moment",3000);
        if (over_pause) end_song_pause();
        return;
    }
    target=lifecycle; asking=1; open_frame=frame;
    ((void (ABI *)(void *, void *))CODE(C_OPEN))(target,NULL); /* openResetSettingWindow */
    printf("Exit prompt: open%s\n",over_pause ? " over the paused song" : "");
}

/* ProcessLifecyclePS4::Update(this): Yes ends the game once no save is busy; F4 pressed opens the
 * window, pausing a song first. */
static ABI int lifecycle_update(HookRegs *r) {
    ++frame;
    if (ending) {
        if (ending==1 && (!runtime_savedata_busy() || frame-yes_frame>=SAVE_WAIT_FRAMES)) {
            ending=2;
            puts(frame-yes_frame>=SAVE_WAIT_FRAMES ? "Exit prompt: a save is still busy after 10 s, ending anyway"
                                                   : "Exit prompt: ending the game");
            fflush(stdout);
            theatrhythm_gpu_request_quit();
        }
        return 0;
    }
    if (paused_song_frame) {
        if (frame-paused_song_frame<PAUSE_FRAMES) return 0;
        paused_song_frame=0;
        open_prompt((void *)r->rdi,1);
        return 0;
    }
    /* A scene change (the title screen's attract movie) can take the window away unanswered: with
     * no window updating, the prompt is over, and F4 can ask again. */
    if (asking && frame-open_frame>GONE_FRAMES && frame-window_frame>GONE_FRAMES) {
        asking=0;
        puts("Exit prompt: its window closed without an answer");
        end_song_pause();
    }
    if (!theatrhythm_gpu_take_exit_request() || asking) return 0;
    if (frame-stage_frame<=2 && stage && frame-window_frame>2) { /* a song plays: pause it first */
        puts("Exit prompt: pausing the song first");
        paused_stage=stage;
        system_pause(paused_stage,1);
        paused_song_frame=frame;
        return 0;
    }
    open_prompt((void *)r->rdi,0);
    return 0;
}
/* onExecuteResetSettingWindow(this, sender, reaction): for the prompt's target, Yes (Submit, 0) or
 * No (Cancel, 1) answers it; the page's own window goes on into the game's code. */
static ABI int reaction(HookRegs *r) {
    if (!asking || (void *)r->rdi!=target) return 0;
    const unsigned char *data=(const unsigned char *)r->rdx;
    const int32_t type=data && !strcmp(class_name(data),"ReactionData") ? load32(data+0x18) : -1;
    if (type==0 || type==1) {
        asking=0;
        if (type==0) {
            ending=1; yes_frame=frame;
            puts(runtime_savedata_busy() ? "Exit prompt: Yes, waiting for the save to finish" : "Exit prompt: Yes");
        } else {
            puts("Exit prompt: No");
            end_song_pause();
        }
    }
    r->rax=0;
    return 1;
}

int runtime_exit_prompt_open(void) { return asking; }

/* The window's text: "Restore default settings? / Language settings will not be changed." in every
 * language; its buttons keep the game's Yes and No. */
const char *runtime_exit_prompt_translation(const char *term) {
    return asking && !strcmp(term,"Scene/Config/Default/001") ? "Exit the game?" : NULL;
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

int runtime_exit_prompt_install(unsigned char *image, uint64_t module_base, unsigned char *stubs, size_t size, int display_page) {
    module=image+module_base;
    if (!display_page) { puts("Exit prompt: off (needs the Display page's translation hook)"); return -1; }
    if (runtime_game_code_find(sites,SITES,code_at,"Exit prompt") || size<5*STUB_SIZE) {
        puts("Exit prompt: off, F4 does nothing in this game version");
        return -1;
    }
    static const struct { int site; HookFn fn; } hooks[]={
        {H_LIFECYCLE,lifecycle_update},{H_WINDOW,window_update},{H_STAGE,stage_update},{H_REACTION,reaction},
        {H_RESUME,stage_resume}};
    unsigned char *p=stubs;
    for (size_t i=0;i<sizeof(hooks)/sizeof(*hooks);++i) {
        unsigned char *function=CODE(hooks[i].site);
        const unsigned moved=code_at[hooks[i].site].moved;
        const int64_t rel=(int64_t)(p-(function+5));
        if (moved<5 || moved>16 || rel!=(int32_t)rel) { puts("Exit prompt: off (hook out of reach, runner bug)"); return -1; }
        unsigned char *stub=p;
        p=emit_stub(p,hooks[i].fn,function,moved);
        const int32_t rel32=(int32_t)(stub-(function+5));
        function[0]=0xe9; memcpy(function+1,&rel32,4);
        memset(function+5,0x90,moved-5u); /* never run: the stub jumps past them */
    }
    puts("Exit prompt: F4 asks \"Exit the game?\" in the game's own window");
    return 0;
}
