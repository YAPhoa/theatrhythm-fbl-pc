/* The "DLC songs added" window without an empty bonus row. The window shows each pack's bonus items
 * with BonusItemDisplay::SetItemInfo(item, count); a pack with none (custom song packs) got a blank
 * icon and "x0". Its entry jumps to a stub that, for a count of 0 or less, hides the display's
 * GameObject and returns; any other count runs the moved instructions and goes on into it. The
 * site is found by name and bytes (runtime_game_code.c): a game version without that code keeps
 * its own. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>

/* BonusItemDisplay::SetItemInfo: push rbp; mov rbp,rsp; push r15; push r14 (moved), then push r13
 * where the stub returns; Component::get_gameObject(this) and GameObject::SetActive(this, value). */
enum { SET_ITEM_INFO, COMPONENT_GET_GAME_OBJECT, GAME_OBJECT_SET_ACTIVE, SITES };
static const GameSite sites[SITES]={
    {"Game.Scene.WindowAddPurchasedMusic.BonusItemDisplay::SetItemInfo", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 50 80 3d ?? ?? ?? ?? 00 41 89 d6"}},
    {"UnityEngine.Component::get_gameObject", 0, 0, 0, {""}},
    {"UnityEngine.GameObject::SetActive", 0, 0, 0, {""}},
};

static unsigned char *emit(unsigned char *p, const void *bytes, size_t size) { memcpy(p,bytes,size); return p+size; }

int runtime_dlc_bonus_install(unsigned char *image, uint64_t module_base, unsigned char *stubs, size_t size) {
    unsigned char *module=image+module_base;
    GameCode at[SITES];
    if (runtime_game_code_find(sites,SITES,at,"DLC bonus row") || size<96) {
        printf("DLC bonus row: an empty bonus row stays\n");
        return -1;
    }
    unsigned char *site=module+at[SET_ITEM_INFO].offset;
    const unsigned moved=at[SET_ITEM_INFO].moved;
    const uint64_t game_object=(uint64_t)(uintptr_t)(module+at[COMPONENT_GET_GAME_OBJECT].offset),
                   set_active=(uint64_t)(uintptr_t)(module+at[GAME_OBJECT_SET_ACTIVE].offset),
                   back=(uint64_t)(uintptr_t)(site+moved);
    unsigned char *p=stubs;
    p=emit(p,(const unsigned char[]){0x85,0xd2, 0x7e,(unsigned char)(moved+14)},4); /* test edx,edx; jle hide */
    p=emit(p,site,moved);                                                       /* the moved instructions */
    p=emit(p,(const unsigned char[]){0xff,0x25,0,0,0,0},6); p=emit(p,&back,8);  /* jmp [rip]: into SetItemInfo */
    p=emit(p,(const unsigned char[]){0x48,0x83,0xec,0x08},4);                   /* hide: sub rsp,8 (16-aligned) */
    p=emit(p,(const unsigned char[]){0x48,0xb8},2); p=emit(p,&game_object,8);   /* mov rax,get_gameObject */
    p=emit(p,(const unsigned char[]){0xff,0xd0},2);                             /* call rax (rdi: this) */
    p=emit(p,(const unsigned char[]){0x48,0x85,0xc0, 0x74,0x11},5);             /* test rax,rax; jz done */
    p=emit(p,(const unsigned char[]){0x48,0x89,0xc7, 0x31,0xf6},5);             /* mov rdi,rax; xor esi,esi */
    p=emit(p,(const unsigned char[]){0x48,0xb8},2); p=emit(p,&set_active,8);    /* mov rax,SetActive */
    p=emit(p,(const unsigned char[]){0xff,0xd0},2);                             /* call rax: SetActive(false) */
    p=emit(p,(const unsigned char[]){0x48,0x83,0xc4,0x08, 0xc3},5);             /* done: add rsp,8; ret */
    if ((size_t)(p-stubs)>size) return -1;
    const int32_t jump=(int32_t)(stubs-(site+5));
    site[0]=0xe9; memcpy(site+1,&jump,4);
    memset(site+5,0x90,moved-5); /* never run: the stub jumps past them */
    printf("DLC bonus row: hidden for packs without bonus items\n");
    return 0;
}
