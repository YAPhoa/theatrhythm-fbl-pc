/* The "DLC songs added" window without an empty bonus row. The window shows each pack's bonus items
 * with BonusItemDisplay::SetItemInfo(item, count); a pack with none (custom song packs) got a blank
 * icon and "x0". Its entry jumps to a stub that, for a count of 0 or less, hides the display's
 * GameObject and returns; any other count runs the moved instructions and goes on into it. The
 * site's bytes are checked first, so another game version keeps its own code. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>

/* BonusItemDisplay::SetItemInfo (Il2CppUserAssemblies): push rbp; mov rbp,rsp; push r15; push r14,
 * then push r13 where the stub returns. */
#define SET_ITEM_INFO 0xf51a50
#define COMPONENT_GET_GAME_OBJECT 0x1e278e0 /* UnityEngine.Component::get_gameObject(this) */
#define GAME_OBJECT_SET_ACTIVE 0x1e29580    /* UnityEngine.GameObject::SetActive(this, value) */
static const unsigned char set_item_info[]={0x55, 0x48,0x89,0xe5, 0x41,0x57, 0x41,0x56, 0x41,0x55};
#define MOVED_LENGTH 8

static unsigned char *emit(unsigned char *p, const void *bytes, size_t size) { memcpy(p,bytes,size); return p+size; }

int runtime_dlc_bonus_install(unsigned char *image, uint64_t module_base, unsigned char *stubs, size_t size) {
    unsigned char *module=image+module_base, *site=module+SET_ITEM_INFO;
    if (memcmp(site,set_item_info,sizeof set_item_info) || size<96) {
        printf("DLC bonus row: game code differs at 0x%llx; an empty bonus row stays\n",(unsigned long long)SET_ITEM_INFO);
        return -1;
    }
    const uint64_t game_object=(uint64_t)(uintptr_t)(module+COMPONENT_GET_GAME_OBJECT),
                   set_active=(uint64_t)(uintptr_t)(module+GAME_OBJECT_SET_ACTIVE),
                   back=(uint64_t)(uintptr_t)(site+MOVED_LENGTH);
    unsigned char *p=stubs;
    p=emit(p,(const unsigned char[]){0x85,0xd2, 0x7e,0x16},4);                  /* test edx,edx; jle hide */
    p=emit(p,set_item_info,MOVED_LENGTH);                                       /* the moved instructions */
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
    memset(site+5,0x90,MOVED_LENGTH-5); /* never run: the stub jumps past them */
    printf("DLC bonus row: hidden for packs without bonus items\n");
    return 0;
}
