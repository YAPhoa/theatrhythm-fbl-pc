/* Straight from the start-up logo to the title screen (settings.ini skip_opening). After the logo the
 * game always goes to the Opening scene, whose OnReady starts the opening movie (MovieController::Play);
 * when the movie ends, or Options skips it once it has been seen, OpeningMain::endMovie marks it seen
 * (UserInfo.LookedOpeningDemo) and moves on to the title. OnReady's entry jumps to a stub that, the
 * first time after each start, goes to endMovie instead, so the movie never starts; later Opening
 * scenes play as before. The sites are found by name and bytes (runtime_game_code.c): a game version
 * without that code keeps its movie. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>

/* OnReady: push rbp; mov rbp,rsp; push rbx; push rax (moved), then reads its MoviePlay parts. */
enum { OPENING_ON_READY, OPENING_END_MOVIE, SITES };
static const GameSite sites[SITES]={
    {"Game.Scene.Opening.OpeningMain::OnReady", 0, 0, 0,
     {"55 48 89 e5 53 50 | 48 8b 9f 98 00 00 00 80 bb 98 00 00 00 00 74 07 31 ff"}},
    {"Game.Scene.Opening.OpeningMain::endMovie", 0, 0, 0, {"55 48 89 e5 41 56 53 80 3d ?? ?? ?? ?? 00 75 13"}},
};

static unsigned char skipped; /* set by the stub (guest code) at the first Opening scene */

static unsigned char *emit(unsigned char *p, const void *bytes, size_t size) { memcpy(p,bytes,size); return p+size; }

int runtime_skip_opening_install(unsigned char *image, uint64_t module_base, unsigned char *stubs, size_t size) {
    unsigned char *module=image+module_base;
    GameCode at[SITES];
    if (runtime_game_code_find(sites,SITES,at,"Skip opening") || size<64) {
        printf("Skip opening: the opening movie plays\n");
        return -1;
    }
    unsigned char *site=module+at[OPENING_ON_READY].offset;
    const unsigned moved=at[OPENING_ON_READY].moved;
    const uint64_t flag=(uint64_t)(uintptr_t)&skipped,
                   end_movie=(uint64_t)(uintptr_t)(module+at[OPENING_END_MOVIE].offset),
                   back=(uint64_t)(uintptr_t)(site+moved);
    unsigned char *p=stubs;
    p=emit(p,(const unsigned char[]){0x48,0xb8},2); p=emit(p,&flag,8);          /* mov rax,&skipped */
    p=emit(p,(const unsigned char[]){0x80,0x38,0x00, 0x75,15},5);               /* cmp byte [rax],0; jne play */
    p=emit(p,(const unsigned char[]){0xc6,0x00,0x01},3);                        /* mov byte [rax],1 */
    p=emit(p,(const unsigned char[]){0x48,0xb8},2); p=emit(p,&end_movie,8);     /* mov rax,endMovie */
    p=emit(p,(const unsigned char[]){0xff,0xe0},2);                             /* jmp rax: endMovie(this) returns for OnReady */
    p=emit(p,site,moved);                                                       /* play: the moved instructions */
    p=emit(p,(const unsigned char[]){0xff,0x25,0,0,0,0},6); p=emit(p,&back,8);  /* jmp [rip]: into OnReady */
    if ((size_t)(p-stubs)>size) return -1;
    const int32_t jump=(int32_t)(stubs-(site+5));
    site[0]=0xe9; memcpy(site+1,&jump,4);
    memset(site+5,0x90,moved-5); /* never run: the stub jumps past them */
    printf("Skip opening: the start goes from the logo to the title screen\n");
    return 0;
}
