/* Every song in Music Stages, whatever the save has unlocked: the cheat unlock_songs (settings.ini
 * [cheats], set on the game's Options > Cheats page; put in place at the start).
 *
 * The save keeps a record per song (Game.Backup.MusicInfo/MusicRecord) whose Unlocked flag (+0x10)
 * the Series Quests, Rhythmia rewards and DLC packs set. The song lists make an item for every song
 * of the game and leave the locked ones out: the Music Select list (Music Stages) when it is built
 * (MusicSelectMusicListPartsMain's InitializeSetting) and each time it is filtered (filterItem), and
 * the Museum's Music Player list the same way. Each of those four tests is a 4-byte
 * `cmp byte [record+0x10],0`, replaced by `test rsp,rsp; nop`: not zero, so the code goes on as for an
 * unlocked song (for a DLC song that is the same code as before: one whose pack is not installed
 * stays unplayable).
 *
 * Nothing changes in the records, so the save keeps the player's own unlocks and the rest of the
 * game (quests, rewards, achievements, profile card) sees them as they are. One write is taken out
 * for that: MSSaveMusicSelect::Save marks a song cleared in Music Stages as unlocked, which without
 * this patch only ever happens to songs that already are.
 *
 * The sites are found by name and bytes (runtime_game_code.c): a game version without that code
 * keeps its locks. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>

/* Each site: the record's pointer is loaded, then comes the patched instruction (`at` bytes into the
 * pattern) and the branch on its flags (in Save, the PickUp test at +0x13). */
enum { SELECT_INIT, SELECT_FILTER, PLAYER_INIT, PLAYER_FILTER, CLEAR_SAVE, SITES };
static const GameSite sites[SITES]={
    {"Game.Scene.MusicSelect.MusicSelectMusicListPartsMain/<InitializeSetting>d__26::MoveNext", 1, 0, 0,
     {"48 8b 40 18 80 79 10 00 8b 48 1c 0f 84"}},
    {"Game.Scene.MusicSelect.MusicSelectMusicListPartsMain::filterItem", 1, 0, 0,
     {"48 8b 4b 48 48 8b 42 18 80 79 10 00 8b 48 1c 74 07 83 f9 02"}},
    {"Game.Scene.MusicPlayer.MusicPlayerMusicListPartsMain/<InitializeSetting>d__47::MoveNext", 1, 0, 0,
     {"49 8b 47 38 80 78 10 00 0f 84 ?? ?? ?? ?? 49 8b 47 18"}},
    {"Game.Scene.MusicPlayer.MusicPlayerMusicListPartsMain::filterItem", 1, 0, 0,
     {"49 8b 56 38 89 c1 80 7a 10 00 0f 84"}},
    {"Game.Logic.MSSaveMusicSelect::Save", 1, 0, 0,
     {"48 8b 5c c1 30 0f 84 ?? ?? ?? ?? c6 43 10 01 80 7b 13 00"}},
};
static const unsigned char at[SITES]={4,8,4,6,11};

int runtime_unlock_songs_install(unsigned char *image, uint64_t module_base) {
    static const unsigned char unlocked[4]={0x48,0x85,0xe4,0x90};   /* test rsp,rsp; nop: ZF clear */
    static const unsigned char nothing[4]={0x0f,0x1f,0x40,0x00};    /* nop dword [rax] */
    unsigned char *module=image+module_base;
    GameCode found[SITES];
    if (runtime_game_code_find(sites,SITES,found,"Unlock songs")) {
        printf("Unlock songs: the song lists keep the save's locks\n");
        return -1;
    }
    for (int i=0;i<SITES;++i)
        memcpy(module+found[i].offset+at[i],i==CLEAR_SAVE ? nothing : unlocked,4);
    printf("Unlock songs: Music Stages and the Music Player list every song; the save's unlocks stay as they are\n");
    return 0;
}
