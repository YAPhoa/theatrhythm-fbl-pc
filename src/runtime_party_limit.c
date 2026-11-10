/* Fifteen party sets instead of five. The game has no one constant for the count, so each use changes:
 *  - PartyInfo's constructor and save reader end in a hook that extends PartySets to fifteen, with a
 *    default PartySet in each new slot (a save writes whatever length the array has);
 *  - MusicStageInfo._partyNumber is a RangedInt(0,4): its range becomes (0,14);
 *  - Edit Party and the song setup window step with Mathf.Repeat(n, 5.0f). The 5.0f is shared with
 *    unrelated code, so those instructions read a 15.0f from the stub page instead;
 *  - entering Edit Party loads the art of sets 0-4 by index: a hook loads the others;
 *  - Edit Party's "did anything change" check snapshots and compares sets 0-4: the snapshot grows and
 *    the comparison also covers sets 5-14, or edits to them would never reach the save;
 *  - the prefabs hold five page dots and window illustrations, and nothing checks these indexes: both
 *    screens show set % 5 on them (the "SET n" text has the real number);
 *  - the set's number is the localized term "Common/SetNo/<n>", which exists up to 5: above that its
 *    translation is term 1's with the real number in place of the digit;
 *  - a multiplayer room sends the first five sets and the active index into them: it stays below five.
 * Every site is found by name and checked by its bytes first (runtime_game_code.c), all before
 * anything changes: a game version without them keeps its five sets. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>

#define PARTY_COUNT 15
#define ARRAY_COUNT 0x18
#define ARRAY_ITEMS 0x20

/* The game code used (runtime_game_code.c): places patched or read (S_) and functions called (C_). The
 * classes and the runtime's allocators have no names to look up: they come from the code at the sites. */
enum {
    S_CTOR_NEW, S_CTOR_END, S_READER_END, S_PARTY_NUMBER, S_RESERVE_NEW, S_SNAPSHOT_END, S_EQUALS_CALL, S_PRELOAD,
    S_LIGHT, S_MULTI, S_TRANSLATION, S_EDIT_WRAP, S_FOCUS_WRAP, S_CHANGE_WRAP, S_SWITCH_WRAP,
    S_PAGE_0, S_PAGE_1, S_PAGE_2, S_PAGE_3, S_PAGE_4, S_PAGE_5, S_PAGE_6, S_PAGE_7, S_PAGE_8, S_PAGE_9,
    C_SET_CTOR, C_SET_COPY_CTOR, C_EQUALS, SITES
};
static const GameSite sites[SITES]={
    /* From its entry, as CharaUseCount's next to it starts the same; at CTOR_NEW: mov rdi,[PartySet[]];
     * mov esi,5; call array_new; mov rdi,[PartySet]; mov r15,rax; call object_new; mov rdi,rax; mov rbx,rax;
     * call PartySet::.ctor */
    {"Game.Backup.PartyInfo::.ctor", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 41 55 41 54 53 50 80 3d ?? ?? ?? ?? 00 49 89 fe 75 1f 48 8d 3d ?? "
     "?? ?? ?? e8 ?? ?? ?? ?? 48 8d 3d ?? ?? ?? ?? e8 ?? ?? ?? ?? c6 05 ?? ?? ?? ?? 01 48 8b 3d "
     "?? ?? ?? ?? be 05 00 00 00 e8 ?? ?? ?? ?? 48 8b 3d ?? ?? ?? ?? 49 89 c7 e8 ?? ?? ?? ?? 48 "
     "89 c7 48 89 c3 e8 ?? ?? ?? ??"}},
    /* ...; mov [r14+0x20],r15 (this.PartySets); then the end, with add rsp,8; pop rbx at CTOR_END */
    {"Game.Backup.PartyInfo::.ctor", 1, 0, 0, {"49 89 5f 40 4d 89 7e 20 48 83 c4 08 5b 41 5c 41 5d"}},
    /* its last field's store (mov [rcx+0x28],eax), which tells it from the formatters after it; then the
     * end: the stack check, and at READER_END mov rax,[rbp-0x68] (the PartyInfo); add rsp,0x58 */
    {"Game.MessagePackFormatter.Formatters.Game.Backup.PartyInfoFormatter::Deserialize", 1, 0, 0,
     {"48 8b 4d 98 89 41 28 e9 ?? ?? ?? ?? 41 ff 8e 88 00 00 00 48 8b 1d ?? ?? ?? ?? "
      "48 8b 03 48 3b 45 d0 75 ?? 48 8b 45 98 48 83 c4 58 5b 41 5c 41 5d"}},
    /* From its entry, as ConfigInfo's next to it has the same range; at RANGE: mov rcx,(max 4, min 0),
     * stored as the range of _partyNumber */
    {"Game.Backup.MusicStageInfo::.ctor", 0, 0, 0,
     {"55 48 89 e5 41 56 53 80 3d ?? ?? ?? ?? 00 49 89 fe 75 2b 48 8d 3d ?? ?? ?? ?? e8 ?? ?? ?? "
     "?? 48 8d 3d ?? ?? ?? ?? e8 ?? ?? ?? ?? 48 8d 3d ?? ?? ?? ?? e8 ?? ?? ?? ?? c6 05 ?? ?? ?? "
     "?? 01 48 8b 3d ?? ?? ?? ?? e8 ?? ?? ?? ?? 80 3d ?? ?? ?? ?? 00 48 89 c3 75 13 48 8d 3d ?? "
     "?? ?? ?? e8 ?? ?? ?? ?? c6 05 ?? ?? ?? ?? 01 48 8b 3d ?? ?? ?? ?? e8 ?? ?? ?? ?? 48 b9 00 "
     "00 00 00 04 00 00 00 48 89 48 10 48 89 43 10"}},
    /* mov rdi,[PartySet[]]; mov esi,5; call array_new; mov rcx,[PartyEditReserveSaveManager];
     * mov rcx,[rcx+0xb8]; mov [rcx],rax: its _beforePartySets, static field 0 */
    {"Game.Scene.PartyEdit.PartyEditReserveSaveManager::.cctor", 1, 0, 0,
     {"48 8b 3d ?? ?? ?? ?? be 05 00 00 00 e8 ?? ?? ?? ?? 48 8b 0d ?? ?? ?? ?? 48 8b 89 b8 00 00 00 48 89 01"}},
    {"Game.Scene.PartyEdit.PartyEditReserveSaveManager::UpdateBeforePartyData", 1, 0, 0, {"8b 40 2c 89 41 10 48 83 c4 08 5b 41 5c 41 5d"}},
    /* mov rax,[Backup]; mov rdi,rbx; rax = the BackupData, static field 0; rsi = its PartyInfo (0x58) and
     * that one's PartySets (0x20); call equelsPartySets; test al,al */
    {"Game.Scene.PartyEdit.PartyEditReserveSaveManager::ReserveSave", 1, 0, 0,
     {"48 8b 05 ?? ?? ?? ?? 48 89 df 48 8b 80 b8 00 00 00 48 8b 00 48 8b 40 58 48 8b 70 20 e8 ?? ?? ?? ?? 84 c0"}},
    /* mov rdi,[PartyEditAssetLoadManager]; rdi = its instance, static field 0; mov rsi,rbx;
     * call LoadAsync(PartySet): the last of the five; then at PRELOAD_END mov rdi,[slot] */
    {"Game.Scene.PartyEdit.PartyEditMain/<InitializeSetting>d__15::MoveNext", 1, 0, 0,
     {"48 8b 3d ?? ?? ?? ?? 48 8b 87 b8 00 00 00 48 8b 38 48 89 de e8 ?? ?? ?? ?? 48 8b 3d ?? ?? ?? ?? 48 8b 1d ?? ?? ?? ??"}},
    /* Indicator::SwitchLight inlined: r12 = lights, rax = the set; mov rbx,lights[rax] at LIGHT_AT */
    {"Game.Scene.WindowSetup.PartyListBehaviour/<updatePartySet>d__30::MoveNext", 1, 0, 0,
     {"4d 8b 66 18 48 8b 45 a8 49 8b 5c c4 20 48 8b 05 ?? ?? ?? ??"}},
    /* the BackupData's MusicStageInfo (0x30), its _partyNumber (0x20): range 0x10, value 0x18; then at
     * MULTI_AT mov esi,[rdx+0x10]; mov edx,[rdx+0x14], the range of ActivePartyIndex */
    {"Game.Scene.MultiBattleCommon.DummyRoomData::AddMemberOwn", 1, 0, 0,
     {"48 8b 49 30 48 8b 49 20 48 8b 51 10 8b 49 18 8b 72 10 8b 52 14 39 ca"}},
    /* up to its read of the term: mov rbx,[r14+0x10] */
    {"Gummi.Localization.UI.TermInfo::GetTranslation", 0, 0, 0,
     {"55 48 89 e5 41 57 | 41 56 41 54 53 48 83 ec 30 4c 8b 25 ?? ?? ?? ?? 49 89 fe 49 8b 04 24 48 89 45 d8 "
      "80 3d ?? ?? ?? ?? 00 75 37 48 8d 3d ?? ?? ?? ?? e8 ?? ?? ?? ?? 48 8d 3d ?? ?? ?? ?? e8 ?? ?? ?? ?? "
      "48 8d 3d ?? ?? ?? ?? e8 ?? ?? ?? ?? 48 8d 3d ?? ?? ?? ?? e8 ?? ?? ?? ?? c6 05 ?? ?? ?? ?? 01 "
      "48 8b 3d ?? ?? ?? ?? 49 8b 5e 10"}},
    /* Mathf.Repeat(n, 5.0f), inlined twice in most of them: see `wraps` */
    {"Game.Scene.PartyEdit.PartyEditPartySetPartsMain/<>c__DisplayClass32_0/<<OnExecutedScenePartsMain>g__changePartySet|0>d::MoveNext", 1, 0, 0,
     {"c5 fa 10 05 ?? ?? ?? ?? c5 f2 5e d0 c4 e3 69 0a d2 09 c5 ea 59 d0 c5 f2 5c d2 c5 f0 57 c9 c5 f8 2e ca 77 12 "
      "c5 f8 2e 15 ?? ?? ?? ??"}},
    {"Game.Scene.WindowSetup.PartyListBehaviour::OnSetFocus", 1, 0, 0,
     {"c5 fa 10 0d ?? ?? ?? ?? 48 89 fb 8b 47 68 8d 48 ff c5 fa 2a c1 c5 fa 5e d1 c4 e3 69 0a d2 09 c5 ea 59 d1 "
      "c5 fa 5c da c5 f8 57 c0 c5 e8 57 d2 c5 f8 2e c3 77 12 c5 f8 2e 1d ?? ?? ?? ?? c5 f8 28 d3 76 04 c5 f8 28 d1 "
      "ff c0 c5 fa 2c f2 c5 da 2a d0 c5 ea 5e d9 c4 e3 61 0a db 09 c5 e2 59 c9 c5 ea 5c c9 c5 f8 2e c1 77 16 "
      "c5 f8 2e 0d ?? ?? ?? ?? c5 f8 28 c1 76 08 c5 fa 10 05 ?? ?? ?? ??"}},
    {"Game.Scene.WindowSetup.PartyListBehaviour/<OnChangeParty>d__29::MoveNext", 1, 0, 0,
     {"c5 fa 10 0d ?? ?? ?? ?? c5 fa 5e d1 c4 e3 69 0a d2 09 c5 ea 59 d1 c5 fa 5c da c5 f8 57 c0 c5 e8 57 d2 "
      "c5 f8 2e c3 77 12 c5 f8 2e 1d ?? ?? ?? ?? c5 f8 28 d3 76 04 c5 f8 28 d1 c5 fa 2c c2 41 89 46 68 "
      "c4 c1 5a 2a 55 28 c5 ea 5e d9 c4 e3 61 0a db 09 c5 e2 59 c9 c5 ea 5c c9 c5 f8 2e c1 77 16 "
      "c5 f8 2e 0d ?? ?? ?? ?? c5 f8 28 c1 76 08 c5 fa 10 05 ?? ?? ?? ??"}},
    {"Game.Scene.WindowSetup.WindowSetupBodyPartsMain::OnSwitchFocus", 1, 0, 0,
     {"c5 fa 10 0d ?? ?? ?? ?? 8b 43 68 8d 48 ff c5 fa 2a c1 c5 fa 5e d1 c4 e3 69 0a d2 09 c5 ea 59 d1 "
      "c5 fa 5c da c5 f8 57 c0 c5 e8 57 d2 c5 f8 2e c3 77 12 c5 f8 2e 1d ?? ?? ?? ?? c5 f8 28 d3 76 04 c5 f8 28 d1 "
      "ff c0 c5 fa 2c f2 c5 da 2a d0 c5 ea 5e d9 c4 e3 61 0a db 09 c5 e2 59 c9 c5 ea 5c c9 c5 f8 2e c1 77 16 "
      "c5 f8 2e 0d ?? ?? ?? ?? c5 f8 28 c1 76 08 c5 fa 10 05 ?? ?? ?? ??"}},
    /* Edit Party's UpdatePartySet, for each dot (0xb0) and illustration (0xb8) of the five: the array,
     * mov ebx,[r13+_selectedPartyIndex] at PAGE_AT, the item. The real index is read separately for
     * the party and the text. */
    {"Game.Scene.PartyEdit.PartyEditPartySetPartsMain/<UpdatePartySet>d__38::MoveNext", 1, 0, 0, {"49 8b 85 b0 00 00 00 41 8b 9d e8 00 00 00 4c 8b 70 20"}},
    {"Game.Scene.PartyEdit.PartyEditPartySetPartsMain/<UpdatePartySet>d__38::MoveNext", 1, 0, 0, {"49 8b 85 b8 00 00 00 41 8b 9d e8 00 00 00 4c 8b 70 20"}},
    {"Game.Scene.PartyEdit.PartyEditPartySetPartsMain/<UpdatePartySet>d__38::MoveNext", 1, 0, 0, {"49 8b 85 b0 00 00 00 41 8b 9d e8 00 00 00 4c 8b 70 28"}},
    {"Game.Scene.PartyEdit.PartyEditPartySetPartsMain/<UpdatePartySet>d__38::MoveNext", 1, 0, 0, {"49 8b 85 b8 00 00 00 41 8b 9d e8 00 00 00 4c 8b 70 28"}},
    {"Game.Scene.PartyEdit.PartyEditPartySetPartsMain/<UpdatePartySet>d__38::MoveNext", 1, 0, 0, {"49 8b 85 b0 00 00 00 41 8b 9d e8 00 00 00 4c 8b 70 30"}},
    {"Game.Scene.PartyEdit.PartyEditPartySetPartsMain/<UpdatePartySet>d__38::MoveNext", 1, 0, 0, {"49 8b 85 b8 00 00 00 41 8b 9d e8 00 00 00 4c 8b 70 30"}},
    {"Game.Scene.PartyEdit.PartyEditPartySetPartsMain/<UpdatePartySet>d__38::MoveNext", 1, 0, 0, {"49 8b 85 b0 00 00 00 41 8b 9d e8 00 00 00 4c 8b 70 38"}},
    {"Game.Scene.PartyEdit.PartyEditPartySetPartsMain/<UpdatePartySet>d__38::MoveNext", 1, 0, 0, {"49 8b 85 b8 00 00 00 41 8b 9d e8 00 00 00 4c 8b 70 38"}},
    {"Game.Scene.PartyEdit.PartyEditPartySetPartsMain/<UpdatePartySet>d__38::MoveNext", 1, 0, 0, {"49 8b 85 b0 00 00 00 41 8b 9d e8 00 00 00 4c 8b 70 40"}},
    {"Game.Scene.PartyEdit.PartyEditPartySetPartsMain/<UpdatePartySet>d__38::MoveNext", 1, 0, 0, {"49 8b 85 b8 00 00 00 41 8b 9d e8 00 00 00 4c 8b 70 40"}},
    {"Game.Backup.PartyInfo/PartySet::.ctor(0)", 0, 0, 0, {""}},
    {"Game.Backup.PartyInfo/PartySet::.ctor(1)", 0, 0, 0, {""}},
    {"Game.Scene.PartyEdit.PartyEditReserveSaveManager::equelsPartySets", 0, 0, 0, {""}},
};
static GameCode code_at[SITES];
/* Where each site's patched instruction starts, and what its pattern reads. */
enum { CTOR_NEW=0x39, CTOR_END=8, READER_END=35, RANGE=0x75, RESERVE_COUNT=8, SNAPSHOT_END=6, EQUALS_CALL=28, PRELOAD_END=25, LIGHT_AT=8, MULTI_AT=15, PAGE_AT=7 };
/* The Mathf.Repeat instructions in those sites: vmovss/vucomiss xmm,[rip+rel], with the game's 5.0f. */
static const struct { int site; unsigned char count, at[4]; } wraps[]={
    {S_EDIT_WRAP,2,{0x00,0x24}},                 /* Edit Party, L1/R1 */
    {S_FOCUS_WRAP,4,{0x00,0x35,0x69,0x77}},      /* PartyListBehaviour::OnSetFocus */
    {S_CHANGE_WRAP,4,{0x00,0x28,0x60,0x6e}},     /* PartyListBehaviour::OnChangeParty */
    {S_SWITCH_WRAP,4,{0x00,0x32,0x66,0x74}},     /* WindowSetupBodyPartsMain::OnSwitchFocus */
};

static unsigned char *module;
#define SITE(site,at) (module+code_at[site].offset+(at))
/* From the code at the sites: the classes' slots, and functions without a name of their own. */
static uint64_t array_class, set_class, backup_class, reserve_class, loader_class, array_new, object_new, asset_load;
static void *(ABI *get_translation)(void *, void *); /* the game's, through its first instructions in a stub */

static void *read_ptr(const void *at) { void *v; memcpy(&v,at,sizeof v); return v; }
static void write_ptr(void *at, void *v) { memcpy(at,&v,sizeof v); }
static void *class_static0(uint64_t klass) { return read_ptr(read_ptr((unsigned char *)read_ptr(module+klass)+0xb8)); }
static void *new_object(void) { return ((void *(ABI *)(void *))(module+object_new))(read_ptr(module+set_class)); }
static unsigned char *current_party_sets(void) {
    const unsigned char *data=class_static0(backup_class);
    return read_ptr((unsigned char *)read_ptr(data+0x58)+0x20); /* BackupData.PartyInfo.PartySets */
}

/* PartyInfo.PartySets (+0x20) with fewer than fifteen sets: a longer array with the same sets first. */
static void ABI extend_party_sets(unsigned char *info) {
    unsigned char *old=info ? read_ptr(info+0x20) : NULL;
    int32_t count;
    if (!old) return;
    memcpy(&count,old+ARRAY_COUNT,sizeof count);
    if (count>=PARTY_COUNT) return;
    unsigned char *sets=((void *(ABI *)(void *, int32_t))(module+array_new))(read_ptr(module+array_class),PARTY_COUNT);
    memcpy(sets+ARRAY_ITEMS,old+ARRAY_ITEMS,8*(size_t)count);
    for (int32_t i=count;i<PARTY_COUNT;++i) {
        void *set=new_object();
        ((void (ABI *)(void *))(module+code_at[C_SET_CTOR].offset))(set);
        write_ptr(sets+ARRAY_ITEMS+8*i,set);
    }
    write_ptr(info+0x20,sets);
}

/* The end of UpdateBeforePartyData, which copied sets 0-4 to _beforePartySets: the same for the rest. */
static void ABI snapshot_other_sets(void) {
    unsigned char *before=class_static0(reserve_class), *sets=current_party_sets();
    for (int i=5;i<PARTY_COUNT;++i) {
        void *copy=new_object();
        ((void (ABI *)(void *, void *, void *))(module+code_at[C_SET_COPY_CTOR].offset))(copy,read_ptr(sets+ARRAY_ITEMS+8*i),NULL);
        write_ptr(before+ARRAY_ITEMS+8*i,copy);
    }
}

/* equelsPartySets for fifteen: it reads only items 0-4 at fixed offsets, so it runs on each group of five. */
static unsigned char ABI party_sets_equal(unsigned char *before, unsigned char *sets) {
    for (int i=0;i<PARTY_COUNT;i+=5)
        if (!((unsigned char (ABI *)(void *, void *))(module+code_at[C_EQUALS].offset))(before+8*i,sets+8*i)) return 0;
    return 1;
}

/* PartyEditMain's setup loaded the current set, then each of sets 0-4 that is not current. */
static void ABI load_other_sets(void) {
    const unsigned char *data=class_static0(backup_class), *sets=current_party_sets();
    int32_t current;
    memcpy(&current,(unsigned char *)read_ptr((unsigned char *)read_ptr(data+0x30)+0x20)+0x18,sizeof current);
    void *loader=class_static0(loader_class);
    for (int i=5;i<PARTY_COUNT;++i)
        if (i!=current) ((void (ABI *)(void *, void *))(module+asset_load))(loader,read_ptr(sets+ARRAY_ITEMS+8*i));
}

/* A guest System.String (length at +0x10, UTF-16 from +0x14) with these characters, of the class of the
 * guest string `like`: one per text, in low memory outside the game's heap, so it is never freed. */
static void *number_string(const unsigned char *like, const uint16_t *chars, int32_t length) {
    static struct { uint16_t chars[32]; int32_t length; unsigned char *string; } made[160];
    static unsigned count;
    for (unsigned i=0;i<count;++i)
        if (made[i].length==length && !memcmp(made[i].chars,chars,2*(size_t)length)) return made[i].string;
    if (count==sizeof made/sizeof *made) return NULL;
    unsigned char *s=runtime_low_alloc(0x16+2*(size_t)length);
    if (!s) return NULL;
    memset(s,0,0x14);
    memcpy(s,like,8);
    memcpy(s+0x10,&length,4);
    memcpy(s+0x14,chars,2*(size_t)length);
    memcpy(made[count].chars,chars,2*(size_t)length);
    made[count].length=length; made[count].string=s;
    return made[count++].string;
}

/* The <n> of a guest string "Common/SetNo/<n>", or 0. */
static int set_number_of(const unsigned char *term) {
    static const char prefix[]="Common/SetNo/";
    const int32_t prefix_length=(int32_t)sizeof prefix-1;
    int32_t length;
    if (!term) return 0;
    memcpy(&length,term+0x10,4);
    if (length<=prefix_length || length>prefix_length+2) return 0;
    int number=0;
    for (int32_t i=0;i<length;++i) {
        uint16_t c;
        memcpy(&c,term+0x14+2*i,2);
        if (i<prefix_length) { if (c!=(uint8_t)prefix[i]) return 0; }
        else if (c<'0' || c>'9') return 0;
        else number=number*10+(c-'0');
    }
    return number;
}

/* TermInfo::GetTranslation(this) (the term at +0x10). A set's number above 5 has no term: it is term 1's
 * translation with its first run of digits (ASCII or full width, as the language writes them) replaced,
 * so a text that translates again, on being enabled or on a language change, gets the same. */
static void *ABI translation(unsigned char *info, void *method) {
    const unsigned char *term=read_ptr(info+0x10);
    const int number=set_number_of(term);
    if (number<=5) return get_translation(info,method);

    int32_t length;
    memcpy(&length,term+0x10,4);
    uint16_t chars[16];
    memcpy(chars,term+0x14,2*(size_t)length);
    while (chars[length-1]>='0' && chars[length-1]<='9') --length;
    chars[length++]='1';
    void *first=number_string(term,chars,length);
    if (!first) return get_translation(info,method);
    write_ptr(info+0x10,first);
    const unsigned char *shown=get_translation(info,method);
    write_ptr(info+0x10,(void *)term);

    uint16_t out[32];
    int32_t size=0, shown_length=0;
    if (shown) memcpy(&shown_length,shown+0x10,4);
    int replaced=0;
    for (int32_t i=0;i<shown_length;) {
        uint16_t c;
        memcpy(&c,shown+0x14+2*i,2);
        const uint16_t zero=c>=0xff10 && c<=0xff19 ? 0xff10 : c>='0' && c<='9' ? '0' : 0;
        if (zero && !replaced) {
            if (size+2>(int32_t)(sizeof out/sizeof *out)) break;
            if (number>9) out[size++]=(uint16_t)(zero+number/10);
            out[size++]=(uint16_t)(zero+number%10);
            replaced=1;
            for (;i<shown_length;++i) { /* the rest of this run */
                memcpy(&c,shown+0x14+2*i,2);
                if (!((c>=0xff10 && c<=0xff19) || (c>='0' && c<='9'))) break;
            }
            continue;
        }
        if (size==(int32_t)(sizeof out/sizeof *out)) break;
        out[size++]=c;
        ++i;
    }
    if (!replaced) { /* no digit to take the place of: the number alone */
        size=0;
        if (number>9) out[size++]=(uint16_t)('0'+number/10);
        out[size++]=(uint16_t)('0'+number%10);
    }
    void *string=number_string(term,out,size);
    return string ? string : (void *)shown;
}

static unsigned char *emit(unsigned char *p, const void *bytes, size_t size) { memcpy(p,bytes,size); return p+size; }
#define EMIT(p,...) emit(p,(const unsigned char[]){__VA_ARGS__},sizeof((const unsigned char[]){__VA_ARGS__}))
static unsigned char *emit_call(unsigned char *p, const void *function) { /* mov rax,function; call rax */
    const uint64_t address=(uint64_t)(uintptr_t)function;
    p=EMIT(p,0x48,0xb8); p=emit(p,&address,8);
    return EMIT(p,0xff,0xd0);
}
static unsigned char *emit_jump_back(unsigned char *p, const void *target) { /* jmp [rip] */
    const uint64_t address=(uint64_t)(uintptr_t)target;
    p=EMIT(p,0xff,0x25,0,0,0,0);
    return emit(p,&address,8);
}
/* `length` bytes at `site` become a jmp or call (opcode) to `target`; the rest are never run. */
static void write_branch(unsigned char *site, size_t length, unsigned char opcode, const unsigned char *target) {
    const int32_t rel=(int32_t)(target-(site+5));
    site[0]=opcode; memcpy(site+1,&rel,4);
    memset(site+5,0x90,length-5);
}
/* The module offset a rip-relative operand or rel32 in a site's pattern goes to: the four bytes at
 * `disp`, in an instruction that ends at `end`. */
static uint64_t site_target(int site, unsigned disp, unsigned end) {
    int32_t rel;
    memcpy(&rel,SITE(site,disp),4);
    return code_at[site].offset+end+(int64_t)rel;
}

int runtime_party_limit_install(unsigned char *image, uint64_t module_base, unsigned char *stubs, size_t size) {
    enum { STUB_CTOR=0, STUB_READER=64, STUB_PAGE=128, STUB_FIFTEEN=192, STUB_LIGHT=200, STUB_SNAPSHOT=256, STUB_EQUAL=320, STUB_PRELOAD=384, STUB_NUMBER=448, STUB_TRANSLATION=464, STUB_END=488 };
    module=image+module_base;
    if (runtime_game_code_find(sites,SITES,code_at,"Party sets") || size<STUB_END) {
        printf("Party sets: it keeps five\n");
        return -1;
    }
    /* The calls and the 5.0f the patterns leave open. */
    const float five=5.0f;
    int same=site_target(S_CTOR_NEW,CTOR_NEW+39,CTOR_NEW+43)==code_at[C_SET_CTOR].offset &&
             site_target(S_EQUALS_CALL,EQUALS_CALL+1,EQUALS_CALL+5)==code_at[C_EQUALS].offset;
    for (size_t i=0;same && i<sizeof wraps/sizeof *wraps;++i)
        for (unsigned k=0;same && k<wraps[i].count;++k)
            same=!memcmp(module+site_target(wraps[i].site,wraps[i].at[k]+4u,wraps[i].at[k]+8u),&five,4);
    if (!same) {
        printf("Party sets: the code differs in game %s; it keeps five\n",runtime_game_version());
        return -1;
    }
    array_class=site_target(S_CTOR_NEW,CTOR_NEW+3,CTOR_NEW+7);
    array_new=site_target(S_CTOR_NEW,CTOR_NEW+13,CTOR_NEW+17);
    set_class=site_target(S_CTOR_NEW,CTOR_NEW+20,CTOR_NEW+24);
    object_new=site_target(S_CTOR_NEW,CTOR_NEW+28,CTOR_NEW+32);
    reserve_class=site_target(S_RESERVE_NEW,20,24);
    backup_class=site_target(S_EQUALS_CALL,3,7);
    loader_class=site_target(S_PRELOAD,3,7);
    asset_load=site_target(S_PRELOAD,21,25);

    /* PartyInfo's constructor and save reader; both then return as they did. */
    unsigned char *p=stubs+STUB_CTOR, *site=SITE(S_CTOR_END,CTOR_END);
    p=EMIT(p,0x4c,0x89,0xf7);                                  /* mov rdi,r14 */
    p=emit_call(p,extend_party_sets);
    p=emit(p,site,5);                                          /* add rsp,8; pop rbx */
    emit_jump_back(p,site+5);
    write_branch(site,5,0xe9,stubs+STUB_CTOR);

    site=SITE(S_READER_END,READER_END);
    p=stubs+STUB_READER;
    p=emit(p,site,8);                                          /* mov rax,[rbp-0x68]; add rsp,0x58 */
    p=EMIT(p,0x50,0x48,0x89,0xc7);                             /* push rax; mov rdi,rax */
    p=emit_call(p,extend_party_sets);
    p=EMIT(p,0x58);                                            /* pop rax */
    emit_jump_back(p,site+8);
    write_branch(site,8,0xe9,stubs+STUB_READER);

    *SITE(S_PARTY_NUMBER,RANGE+6)=PARTY_COUNT-1; /* the max */

    const float fifteen=PARTY_COUNT;
    memcpy(stubs+STUB_FIFTEEN,&fifteen,4);
    for (size_t i=0;i<sizeof wraps/sizeof *wraps;++i)
        for (unsigned k=0;k<wraps[i].count;++k) {
            site=SITE(wraps[i].site,wraps[i].at[k]);
            const int32_t rel=(int32_t)(stubs+STUB_FIFTEEN-(site+8));
            memcpy(site+4,&rel,4);
        }

    /* The dot and illustration index: ebx = _selectedPartyIndex % 5, every other register kept. */
    p=stubs+STUB_PAGE;
    p=EMIT(p,0x50,0x51,0x52, 0x41,0x8b,0x85,0xe8,0,0,0, 0x31,0xd2, 0xb9,5,0,0,0, 0xf7,0xf1, 0x89,0xd3, 0x5a,0x59,0x58, 0xc3);
    for (int i=S_PAGE_0;i<=S_PAGE_9;++i)
        write_branch(SITE(i,PAGE_AT),7,0xe8,stubs+STUB_PAGE);

    /* The setup window's light: rbx = _onLights[rax % Length]; the next instruction overwrites rax.
     * push rdx; xor edx,edx; div dword [r12+0x18]; mov rbx,[r12+8*rdx+0x20]; pop rdx; ret */
    p=stubs+STUB_LIGHT;
    p=EMIT(p,0x52, 0x31,0xd2, 0x41,0xf7,0x74,0x24,ARRAY_COUNT, 0x49,0x8b,0x5c,0xd4,ARRAY_ITEMS, 0x5a, 0xc3);
    write_branch(SITE(S_LIGHT,LIGHT_AT),5,0xe8,stubs+STUB_LIGHT);

    /* The change check. */
    *SITE(S_RESERVE_NEW,RESERVE_COUNT)=PARTY_COUNT;
    site=SITE(S_SNAPSHOT_END,SNAPSHOT_END);
    p=stubs+STUB_SNAPSHOT;
    p=emit_call(p,snapshot_other_sets);
    p=emit(p,site,5);                                          /* add rsp,8; pop rbx */
    emit_jump_back(p,site+5);
    write_branch(site,5,0xe9,stubs+STUB_SNAPSHOT);
    p=stubs+STUB_EQUAL;
    emit_jump_back(p,(const void *)party_sets_equal);
    write_branch(SITE(S_EQUALS_CALL,EQUALS_CALL),5,0xe8,stubs+STUB_EQUAL);

    /* The art of the sets past the first five. */
    site=SITE(S_PRELOAD,PRELOAD_END);
    p=stubs+STUB_PRELOAD;
    p=emit_call(p,load_other_sets);
    const uint64_t next_slot=(uint64_t)(uintptr_t)(module+site_target(S_PRELOAD,PRELOAD_END+3,PRELOAD_END+7));
    p=EMIT(p,0x48,0xbf); p=emit(p,&next_slot,8);               /* mov rdi,slot */
    p=EMIT(p,0x48,0x8b,0x3f);                                  /* mov rdi,[rdi] */
    emit_jump_back(p,site+7);
    write_branch(site,7,0xe9,stubs+STUB_PRELOAD);

    /* The set's number: GetTranslation enters translation(), which runs the game's through this stub. */
    site=SITE(S_TRANSLATION,0);
    const unsigned moved=code_at[S_TRANSLATION].moved;
    p=stubs+STUB_TRANSLATION;
    p=emit(p,site,moved);
    emit_jump_back(p,site+moved);
    get_translation=(void *(ABI *)(void *, void *))(void *)(stubs+STUB_TRANSLATION);
    emit_jump_back(stubs+STUB_NUMBER,(const void *)translation);
    write_branch(site,moved,0xe9,stubs+STUB_NUMBER);

    /* xor esi,esi; push 4; pop rdx; nop: clamp ActivePartyIndex to 0-4. */
    memcpy(SITE(S_MULTI,MULTI_AT),(const unsigned char[]){0x31,0xf6,0x6a,0x04,0x5a,0x90},6);

    printf("Party sets: fifteen instead of five\n");
    return 0;
}
