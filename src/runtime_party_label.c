/* The set number above the party in the song setup window, at 4K. The label is a UnityEngine.UI.Text
 * 35 units tall with a 26 font and vertical overflow "Truncate": Text drops any line that does not fit,
 * and with render_resolution=4k (canvas scale 2) the generator returns no lines at all, so the window
 * says "SET" with no number. Before updatePartySet sets the text, a stub turns that label's vertical
 * overflow to "Overflow"; it stays centred in its box, so at 1080p nothing moves.
 * The sites are found by name and bytes (runtime_game_code.c): a game version without that code keeps
 * its own. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>

/* <updatePartySet>d__30::MoveNext: _partyNumberText.SetText(term): mov rdx,[method]; mov rdi,r14; mov rsi,rax;
 * call LocalizedTextBase<T>::SetText (shared generic code, no method of its own), at LABEL_CALL; then the set's
 * index, [rax+0x68]. UnityEngine.UI.Text::get_verticalOverflow: mov rax,[rdi+m_FontData];
 * mov eax,[rax+m_VerticalOverflow], the fields the stub writes. */
enum { SET_TEXT_CALL, GET_VERTICAL_OVERFLOW, SITES };
static const GameSite sites[SITES]={
    {"Game.Scene.WindowSetup.PartyListBehaviour/<updatePartySet>d__30::MoveNext", 1, 0, 0,
     {"48 8b 15 ?? ?? ?? ?? 4c 89 f7 48 89 c6 e8 ?? ?? ?? ?? 48 8b 45 b0 4c 8b 70 38 48 63 40 68"}},
    {"UnityEngine.UI.Text::get_verticalOverflow", 0, 0, 0, {"48 8b 87 d0 00 00 00 8b 40 38"}},
};
enum { LABEL_CALL=13 };

int runtime_party_label_install(unsigned char *image, uint64_t module_base, unsigned char *stubs, size_t size) {
    static const unsigned char set_overflow[]={
        0x48,0x8b,0x87,0xd0,0x00,0x00,0x00,       /* mov rax,[rdi+m_FontData] */
        0xc7,0x40,0x38,0x01,0x00,0x00,0x00,       /* mov dword [rax+m_VerticalOverflow],1 (Overflow) */
    };
    unsigned char *module=image+module_base;
    GameCode at[SITES];
    if (runtime_game_code_find(sites,SITES,at,"Party label") || size<sizeof set_overflow+5) {
        printf("Party label: the set number stays blank at 4K\n");
        return -1;
    }
    unsigned char *site=module+at[SET_TEXT_CALL].offset+LABEL_CALL;
    int32_t rel;
    memcpy(&rel,site+1,4);
    unsigned char *target=site+5+rel;
    memcpy(stubs,set_overflow,sizeof set_overflow);
    unsigned char *jump=stubs+sizeof set_overflow;
    rel=(int32_t)(target-(jump+5));
    jump[0]=0xe9; /* jmp LocalizedTextBase::SetText */
    memcpy(jump+1,&rel,4);
    rel=(int32_t)(stubs-(site+5));
    memcpy(site+1,&rel,4);
    printf("Party label: the song setup window's set number may overflow its box\n");
    return 0;
}
