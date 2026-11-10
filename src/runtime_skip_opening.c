/* Straight to the title screen (the cheat skip_opening: settings.ini [cheats], Options > Cheats): no start-up logos, no opening movie.
 *
 * Logos: StartUpLogoMain plays a timeline (Final Fantasy 35th anniversary, Square Enix, indieszero).
 * Its markers reach OnTimelineEvent: 0/1 open and close a logo's skip window (+0xb4; OnUpdate jumps
 * the director to that logo's skip time when a button is pressed in it, and sets +0xb5 "skipped"), 3
 * starts a sound that runs under the logos, 5 sets the director's time to the timeline's end (+0xa8),
 * whose markers then fire 2: stop the sounds (with a 1 s fade) if a logo was skipped, pick the next
 * scene (first-time setup or Opening) and leave. OnUpdate's entry jumps to a stub that, while the
 * scene is not marked skipped (its first frame, before the director has fired any marker), marks it
 * and runs marker 5's code: the director jumps to the end and the scene ends as after a button skip.
 * OnTimelineEvent's entry jumps to a stub that drops marker 3 of a skipped scene, so the sound never
 * starts. Later frames run OnUpdate as before.
 *
 * Opening: after the logos the game goes to the Opening scene, whose OnReady starts the opening movie
 * (MovieController::Play); when the movie ends, or Options skips it once it has been seen,
 * OpeningMain::endMovie marks it seen (UserInfo.LookedOpeningDemo) and moves on to the title.
 * OnReady's entry jumps to a stub that, the first time after each start, goes to endMovie instead,
 * so the movie never starts; later Opening scenes play as before.
 *
 * The sites are found by name and bytes (runtime_game_code.c): a game version without that code
 * keeps its logos or its movie. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>

/* OnReady: push rbp; mov rbp,rsp; push rbx; push rax (moved), then reads its MoviePlay parts. */
enum { OPENING_ON_READY, OPENING_END_MOVIE, OPENING_SITES };
static const GameSite opening_sites[OPENING_SITES]={
    {"Game.Scene.Opening.OpeningMain::OnReady", 0, 0, 0,
     {"55 48 89 e5 53 50 | 48 8b 9f 98 00 00 00 80 bb 98 00 00 00 00 74 07 31 ff"}},
    {"Game.Scene.Opening.OpeningMain::endMovie", 0, 0, 0, {"55 48 89 e5 41 56 53 80 3d ?? ?? ?? ?? 00 75 13"}},
};

/* OnUpdate: push rbp; mov rbp,rsp; push r14 (moved). OnTimelineEvent: push rbp; mov rbp,rsp; push r15
 * (moved), then push r14; push rbx; push rax: its frame; marker 5's code (time = this+0xa8, director
 * this+0x88, then to the shared epilogue); markers 0, 1 and 2, which use the skip fields +0xb4, +0xb0
 * and +0xb5; and the jump on the marker's type (+0x14, 0 to 5). */
enum { LOGO_ON_UPDATE, LOGO_EVENT, LOGO_TO_END, LOGO_FIELDS, LOGO_KIND, LOGO_SITES };
static const GameSite logo_sites[LOGO_SITES]={
    {"Game.Scene.StartUpLogo.StartUpLogoMain::OnUpdate", 0, 0, 0,
     {"55 48 89 e5 41 56 | 53 48 83 ec 10 80 3d ?? ?? ?? ?? 00 48 89 fb 75 13 48 8d 3d ?? ?? ?? ??"}},
    {"Game.Scene.StartUpLogo.StartUpLogoMain::OnTimelineEvent", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 53 50 80 3d ?? ?? ?? ?? 00 48 89 f3 49 89 fe"}},
    {"Game.Scene.StartUpLogo.StartUpLogoMain::OnTimelineEvent", 1, 0, 0,
     {"c4 c1 7b 10 86 a8 00 00 00 48 8b 05 ?? ?? ?? ?? 4d 8b be 88 00 00 00 48 85 c0 75 26"}},
    {"Game.Scene.StartUpLogo.StartUpLogoMain::OnTimelineEvent", 1, 0, 0,
     {"41 c6 86 b4 00 00 00 01 eb ?? 41 c6 86 b4 00 00 00 00 41 ff 86 b0 00 00 00 eb ?? 41 80 be b5 00 00 00 00"}},
    {"Game.Scene.StartUpLogo.StartUpLogoMain::OnTimelineEvent", 1, 0, 0, {"8b 43 14 48 83 f8 05 77 ??"}},
};

static unsigned char skipped;   /* set by the opening stub (guest code) at the first Opening scene */

static unsigned char *emit(unsigned char *p, const void *bytes, size_t size) { memcpy(p,bytes,size); return p+size; }

/* jmp rel32 from `site` to `stub`; the rest of the moved bytes become nops (the stub jumps past them). */
static void hook(unsigned char *site, unsigned moved, unsigned char *stub) {
    const int32_t jump=(int32_t)(stub-(site+5));
    site[0]=0xe9; memcpy(site+1,&jump,4);
    memset(site+5,0x90,moved-5);
}

static unsigned char *opening_stub(unsigned char *module, const GameCode *at, unsigned char *p) {
    unsigned char *site=module+at[OPENING_ON_READY].offset;
    const unsigned moved=at[OPENING_ON_READY].moved;
    const uint64_t flag=(uint64_t)(uintptr_t)&skipped,
                   end_movie=(uint64_t)(uintptr_t)(module+at[OPENING_END_MOVIE].offset),
                   back=(uint64_t)(uintptr_t)(site+moved);
    unsigned char *stub=p;
    p=emit(p,(const unsigned char[]){0x48,0xb8},2); p=emit(p,&flag,8);          /* mov rax,&skipped */
    p=emit(p,(const unsigned char[]){0x80,0x38,0x00, 0x75,15},5);               /* cmp byte [rax],0; jne play */
    p=emit(p,(const unsigned char[]){0xc6,0x00,0x01},3);                        /* mov byte [rax],1 */
    p=emit(p,(const unsigned char[]){0x48,0xb8},2); p=emit(p,&end_movie,8);     /* mov rax,endMovie */
    p=emit(p,(const unsigned char[]){0xff,0xe0},2);                             /* jmp rax: endMovie(this) returns for OnReady */
    p=emit(p,site,moved);                                                       /* play: the moved instructions */
    p=emit(p,(const unsigned char[]){0xff,0x25,0,0,0,0},6); p=emit(p,&back,8);  /* jmp [rip]: into OnReady */
    hook(site,moved,stub);
    return p;
}

static unsigned char *logo_stubs(unsigned char *module, const GameCode *at, unsigned char *p) {
    unsigned char *update=module+at[LOGO_ON_UPDATE].offset, *event=module+at[LOGO_EVENT].offset;
    const unsigned update_moved=at[LOGO_ON_UPDATE].moved, event_moved=at[LOGO_EVENT].moved;
    const uint64_t to_end=(uint64_t)(uintptr_t)(module+at[LOGO_TO_END].offset),
                   update_back=(uint64_t)(uintptr_t)(update+update_moved),
                   event_back=(uint64_t)(uintptr_t)(event+event_moved);
    unsigned char *stub=p;
    p=emit(p,(const unsigned char[]){0x80,0xbf,0xb5,0,0,0,0x00, 0x75,32},9);    /* cmp byte [rdi+0xb5],0; jne update */
    p=emit(p,(const unsigned char[]){0xc6,0x87,0xb5,0,0,0,0x01},7);             /* mov byte [rdi+0xb5],1: skipped */
    p=emit(p,(const unsigned char[]){0x55, 0x48,0x89,0xe5, 0x41,0x57, 0x41,0x56, 0x53, 0x50},10); /* OnTimelineEvent's frame */
    p=emit(p,(const unsigned char[]){0x49,0x89,0xfe},3);                        /* mov r14,rdi: this */
    p=emit(p,(const unsigned char[]){0x48,0xb8},2); p=emit(p,&to_end,8);        /* mov rax,marker 5's code */
    p=emit(p,(const unsigned char[]){0xff,0xe0},2);                             /* jmp rax: its epilogue returns for OnUpdate */
    p=emit(p,update,update_moved);                                              /* update: the moved instructions */
    p=emit(p,(const unsigned char[]){0xff,0x25,0,0,0,0},6); p=emit(p,&update_back,8); /* jmp [rip]: into OnUpdate */
    hook(update,update_moved,stub);
    stub=p;
    p=emit(p,(const unsigned char[]){0x80,0xbf,0xb5,0,0,0,0x00, 0x74,15},9);    /* cmp byte [rdi+0xb5],0; je event */
    p=emit(p,(const unsigned char[]){0x48,0x85,0xf6, 0x74,10},5);               /* test rsi,rsi; je event */
    p=emit(p,(const unsigned char[]){0x83,0x7e,0x14,0x03, 0x75,4},6);           /* cmp dword [rsi+0x14],3; jne event */
    p=emit(p,(const unsigned char[]){0x48,0x89,0xf0, 0xc3},4);                  /* mov rax,rsi; ret: the sound is dropped */
    p=emit(p,event,event_moved);                                                /* event: the moved instructions */
    p=emit(p,(const unsigned char[]){0xff,0x25,0,0,0,0},6); p=emit(p,&event_back,8); /* jmp [rip]: into OnTimelineEvent */
    hook(event,event_moved,stub);
    return p;
}

int runtime_skip_opening_install(unsigned char *image, uint64_t module_base, unsigned char *stubs, size_t size) {
    unsigned char *module=image+module_base, *p=stubs;
    GameCode opening[OPENING_SITES], logo[LOGO_SITES];
    int installed=0;
    if (size<192) return -1;
    if (!runtime_game_code_find(logo_sites,LOGO_SITES,logo,"Skip opening")) {
        p=logo_stubs(module,logo,p);
        printf("Skip opening: no start-up logos\n");
        installed=1;
    } else {
        printf("Skip opening: the start-up logos play\n");
    }
    if (!runtime_game_code_find(opening_sites,OPENING_SITES,opening,"Skip opening")) {
        p=opening_stub(module,opening,p);
        printf("Skip opening: the start goes from the logos to the title screen\n");
        installed=1;
    } else {
        printf("Skip opening: the opening movie plays\n");
    }
    return installed && (size_t)(p-stubs)<=size ? 0 : -1;
}
