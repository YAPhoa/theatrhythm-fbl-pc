/* The party's walk on field stages at any frame rate. StageCharacter::MyLateUpdate picks run, walk or
 * idle from the squared distance the character moved since the last frame (run from 4.9e-5, walk
 * above 1e-6), thresholds made for 60 frames a second: uncapped, the party slides along a field in
 * its idle pose. Its comparison jumps to a stub that first divides that distance by (60 dt)^2 (dt:
 * Time.deltaTime), the distance it would have moved in a 60th of a second; at 60 fps it is the same.
 * The site's bytes are checked first, so another game version keeps its own code. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>

/* MyLateUpdate: vaddss xmm0,xmm1,xmm0 (the squared distance); vucomiss xmm0,[rip+run threshold],
 * then jae run. The stub ends with the comparison and jumps back to the jae. */
#define WALK_SITE 0x1d50992
#define WALK_RUN_THRESHOLD 0x4617020
#define TIME_GET_DELTA_TIME 0x1e32990 /* UnityEngine.Time::get_deltaTime */
static const unsigned char run_threshold[]={0x5a,0x85,0x4d,0x38}; /* the game's 4.9e-5f */
static const unsigned char walk_site[]={0xc5,0xf2,0x58,0xc0, 0xc5,0xf8,0x2e,0x05,0x82,0x66,0x8c,0x02, 0x73,0x60};
#define WALK_SITE_LENGTH 12

static unsigned char *emit(unsigned char *p, const void *bytes, size_t size) { memcpy(p,bytes,size); return p+size; }
/* A rip-relative disp32 at `p` (the instruction ends `end` bytes after p) to `target`. */
static unsigned char *rel32(unsigned char *p, int end, const unsigned char *target) {
    const int32_t rel=(int32_t)(target-(p+end));
    return emit(p,&rel,4);
}

int runtime_field_walk_install(unsigned char *image, uint64_t module_base, unsigned char *stubs, size_t size) {
    unsigned char *module=image+module_base, *site=module+WALK_SITE;
    if (memcmp(site,walk_site,sizeof walk_site) || memcmp(module+WALK_RUN_THRESHOLD,run_threshold,4) || size<160) {
        printf("Field walk: game code differs at 0x%llx; the walk follows the frame rate\n",(unsigned long long)WALK_SITE);
        return -1;
    }
    unsigned char *data=stubs+128, *p=stubs; /* data: 60.0f, the run threshold */
    const float sixty=60.0f;
    memcpy(data,&sixty,4); memcpy(data+4,run_threshold,4);
    const uint64_t delta_time=(uint64_t)(uintptr_t)(module+TIME_GET_DELTA_TIME), back=(uint64_t)(uintptr_t)(site+WALK_SITE_LENGTH);
    p=emit(p,(const unsigned char[]){0xf3,0x0f,0x58,0xc1},4);                   /* addss xmm0,xmm1: the distance */
    p=emit(p,(const unsigned char[]){0x48,0x83,0xec,0x40},4);                   /* sub rsp,0x40 (rsp stays 16-aligned) */
    p=emit(p,(const unsigned char[]){0x0f,0x11,0x04,0x24},4);                   /* movups [rsp],xmm0 */
    p=emit(p,(const unsigned char[]){0x0f,0x11,0x54,0x24,0x10},5);              /* movups [rsp+0x10],xmm2 (position xy) */
    p=emit(p,(const unsigned char[]){0x0f,0x11,0x5c,0x24,0x20},5);              /* movups [rsp+0x20],xmm3 (position z) */
    p=emit(p,(const unsigned char[]){0x48,0xb8},2); p=emit(p,&delta_time,8);    /* mov rax,get_deltaTime */
    p=emit(p,(const unsigned char[]){0xff,0xd0},2);                             /* call rax: xmm0 = dt */
    p=emit(p,(const unsigned char[]){0x0f,0x10,0x0c,0x24},4);                   /* movups xmm1,[rsp] */
    p=emit(p,(const unsigned char[]){0xf3,0x0f,0x59,0x05},4); p=rel32(p,4,data); /* mulss xmm0,[60.0] */
    p=emit(p,(const unsigned char[]){0x0f,0x57,0xd2, 0x0f,0x2e,0xc2, 0x76,0x08},8); /* xorps xmm2,xmm2; ucomiss xmm0,xmm2; jbe keep */
    p=emit(p,(const unsigned char[]){0xf3,0x0f,0x59,0xc0, 0xf3,0x0f,0x5e,0xc8},8); /* mulss xmm0,xmm0; divss xmm1,xmm0 */
    p=emit(p,(const unsigned char[]){0x0f,0x28,0xc1},3);                        /* keep: movaps xmm0,xmm1 */
    p=emit(p,(const unsigned char[]){0x0f,0x10,0x54,0x24,0x10},5);              /* movups xmm2,[rsp+0x10] */
    p=emit(p,(const unsigned char[]){0x0f,0x10,0x5c,0x24,0x20},5);              /* movups xmm3,[rsp+0x20] */
    p=emit(p,(const unsigned char[]){0x48,0x83,0xc4,0x40},4);                   /* add rsp,0x40 */
    p=emit(p,(const unsigned char[]){0x0f,0x2e,0x05},3); p=rel32(p,4,data+4);  /* ucomiss xmm0,[run threshold] */
    p=emit(p,(const unsigned char[]){0xff,0x25,0,0,0,0},6); p=emit(p,&back,8);  /* jmp [rip]: the jae */
    if (p>data) return -1;
    const int32_t jump=(int32_t)(stubs-(site+5));
    site[0]=0xe9; memcpy(site+1,&jump,4);
    memset(site+5,0x90,WALK_SITE_LENGTH-5); /* never run: the stub jumps past them */
    printf("Field walk: the party's walk and run follow the frame time\n");
    return 0;
}
