/* Names for party sets on the Party Edit screen, shown centred above "Set N" by a copy of the set's
 * number text, whose term (one per set, ours) the translation hook answers with the name. L2 (which the
 * screen leaves unused, and the bottom bar shows a guide for) names the set in the game's own window: the
 * game opens its Optimize window, whose message is swapped through the translation hook for the name being
 * typed, and whose answer is taken here, so Optimize never runs. Typing is SDL text input without a
 * dialog of the runner's: a keyboard, or Steam's on-screen keyboard, which SDL brings up. Enter accepts
 * and Esc cancels, and the window's own buttons do the same. Names live in party_names.txt in the save
 * folder ("<N><tab><name>" lines, UTF-8), so the game's save is untouched. Every site is found by name
 * and checked by its bytes first (runtime_game_code.c): a game version without them keeps its own screen. */
#include "runtime.h"
#include "gpu/theatrhythm_gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The game code used (runtime_game_code.c): hooked functions (H_) and functions called, here under the
 * names GUEST takes. */
enum {
    H_LAYER_UPDATE, H_HELP_DRAW, H_END, H_ANSWER, H_EVENT, H_GRID_SET_FOCUS, H_GRID_LOST_FOCUS, H_GRID_REMOVE_FOCUS,
    S_LABEL, TEXT_RETRANSLATE_SITE, OPEN_WINDOW, GET_HELP_DATA,
    COMPONENT_TRANSFORM, TRANSFORM_PARENT, COMPONENT_GAME_OBJECT, OBJECT_ALIVE, OBJECT_DESTROY, CLONE_WITH_PARENT,
    RECT_SET_ANCHORED, RECT_GET_SIZE, RECT_SET_SIZE,
    TEXT_SET_ALIGNMENT, TEXT_GET_FONT_SIZE, TEXT_SET_FONT_SIZE, TEXT_SET_H_OVERFLOW, TEXT_SET_V_OVERFLOW, SITES
};
static const GameSite sites[SITES]={
    {"Gummi.Scene.Layer.Layer::Update", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 53 48 83 ec 28 4c 8b 3d ?? ?? ?? ?? 48 89 fb 49 8b 07 48 89 45 e0"}},
    {"Game.Scene.Common.CommonMenu.HelpBar::Draw", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 48 81 ec f8 00 00 00 48 8b 05 ?? ?? ?? ?? 48 89 b5 30 ff ff ff "
      "48 89 bd 58 ff ff ff 48 8b 00 48 89 45 d0"}},
    {"Game.Scene.PartyEdit.PartyEditMain::End", 0, 0, 0,
     {"55 48 89 e5 41 56 53 | 48 83 e4 e0 48 83 ec 40 4c 8b 35 ?? ?? ?? ?? 48 89 fb 49 8b 06 48 89 44 24 38"}},
    /* the answer, up to cmp dword [rsi+0x18],0 (the first button); sete [r14+0x10] */
    {"Game.Scene.PartyEdit.PartyEditPartySetPartsMain/<>c__DisplayClass32_1::<OnExecutedScenePartsMain>b__2", 0, 0, 0,
     {"55 48 89 e5 41 56 53 | 80 3d ?? ?? ?? ?? 00 48 89 d3 49 89 fe 75 13 48 8d 3d ?? ?? ?? ?? e8 ?? ?? ?? ?? "
      "c6 05 ?? ?? ?? ?? 01 48 8b 05 ?? ?? ?? ?? 48 8b 0b 31 f6 0f b6 90 2c 01 00 00 48 8b 89 c8 00 00 00 "
      "48 39 44 d1 f8 48 0f 44 f3 83 7e 18 00 41 0f 94 46 10"}},
    /* the events: cmp esi,3 (closed); jne; the opener's class (0x18), the screen (0x10) */
    {"Game.Scene.PartyEdit.PartyEditPartySetPartsMain/<>c__DisplayClass32_1::<OnExecutedScenePartsMain>b__3", 0, 0, 0,
     {"55 48 89 e5 41 56 53 | 83 fe 03 75 56 48 8b 47 18 48 89 fb 31 f6 48 8b 40 10 8b b8 e8 00 00 00"}},
    {"Game.Scene.PartyEdit.PartyEditPartySetPartsMain::OnSetFocus", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 53 50 80 3d ?? ?? ?? ?? 00 48 89 f3 49 89 fe 75 13 48 8d 3d ?? ?? ?? ?? "
      "e8 ?? ?? ?? ?? c6 05 ?? ?? ?? ?? 01"}},
    {"Game.Scene.PartyEdit.PartyEditPartySetPartsMain::OnLostFocusReturn", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 53 50 80 3d ?? ?? ?? ?? 00 49 89 f7 49 89 fe 75 13 48 8d 3d ?? ?? ?? ?? "
      "e8 ?? ?? ?? ?? c6 05 ?? ?? ?? ?? 01"}},
    {"Game.Scene.PartyEdit.PartyEditPartySetPartsMain::OnRemoveFocus", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 53 50 80 3d ?? ?? ?? ?? 00 48 89 f3 49 89 fe 74 4b"}},
    /* UpdatePartySet: label.SetText("Common/SetNo/<N>", method): mov rdx,[method]; mov rdi,r14; mov rsi,rax;
     * call LocalizedTextBase<T>::SetText (shared generic code, no method of its own), at LABEL_CALL */
    {"Game.Scene.PartyEdit.PartyEditPartySetPartsMain/<UpdatePartySet>d__38::MoveNext", 1, 0, 0,
     {"48 8b 15 ?? ?? ?? ?? 4c 89 f7 48 89 c6 e8 ?? ?? ?? ?? 49 8b 85 b0 00 00 00"}},
    /* LocalizedTextBase<T>::SetTextByTermInfo (shared, two instances), after the entry's 7 bytes, which
     * the Display page moves to its hook: this finds it with or without that hook. */
    {NULL, 1, 2, 0,
     {"48 8b 46 20 48 89 f3 49 89 fe 48 8b 80 c0 00 00 00 48 8b 70 78 ff 56 08 48 85 c0 74 3d 48 8b 43 20 4c 89 f7 "
      "48 8b 80 c0 00 00 00 48 8b 70 78 ff 56 08 48 89 c7 31 f6 e8 ?? ?? ?? ??"}},
    {"Game.Scene.PartyEdit.PartyEditPartySetPartsMain/<>c__DisplayClass32_0::<OnExecutedScenePartsMain>g__openAutoSetWindow|1", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 18 80 3d ?? ?? ?? ?? 00 48 89 fb"}},
    {"Table.HelpBarTable::GetHelpBarData", 0, 0, 0,
     {"55 48 89 e5 53 50 80 3d ?? ?? ?? ?? 00 48 89 fb 75 13 48 8d 3d ?? ?? ?? ?? e8 ?? ?? ?? ?? c6 05 ?? ?? ?? ?? 01"}},
    {"UnityEngine.Component::get_transform", 0, 0, 0, {""}},
    {"UnityEngine.Transform::get_parent", 0, 0, 0, {""}},
    {"UnityEngine.Component::get_gameObject", 0, 0, 0, {""}},
    {"UnityEngine.Object::op_Implicit", 0, 0, 0, {""}},
    {"UnityEngine.Object::Destroy(1)", 0, 0, 0, {""}},
    {"UnityEngine.Object::Internal_CloneSingleWithParent", 0, 0, 0, {""}},
    {"UnityEngine.RectTransform::set_anchoredPosition", 0, 0, 0, {""}},
    {"UnityEngine.RectTransform::get_sizeDelta", 0, 0, 0, {""}},
    {"UnityEngine.RectTransform::set_sizeDelta", 0, 0, 0, {""}},
    {"UnityEngine.UI.Text::set_alignment", 0, 0, 0, {""}},
    {"UnityEngine.UI.Text::get_fontSize", 0, 0, 0, {""}},
    {"UnityEngine.UI.Text::set_fontSize", 0, 0, 0, {""}},
    {"UnityEngine.UI.Text::set_horizontalOverflow", 0, 0, 0, {""}},
    {"UnityEngine.UI.Text::set_verticalOverflow", 0, 0, 0, {""}},
};
static GameCode code_at[SITES];
enum { LABEL_CALL=13, RETRANSLATE_MOVED=7 };
#define ANCHOR_MIDDLE_CENTER 4
#define WRAP_OVERFLOW 1             /* HorizontalWrapMode.Overflow, VerticalWrapMode.Overflow */
#define NAME_FONT_PERCENT 80
#define NAME_WIDTH 900.0f           /* the name's box, wide enough for any name on one line */
#define NAME_ABOVE 58.0f            /* from the banner to the name, clear of the L1 and R1 prompts */

#define MAX_SETS 32
#define MAX_NAME 32                 /* code points kept */
#define WINDOW_TERM "Scene/PartyEdit/AutoSetWindow"
#define NAME_TERM "Theatrhythm/PartySetName/"
#define GUIDE_TERM "Theatrhythm/PartySetRename"
#define L2_BUTTON 0x100
/* The bottom bar's guides: rows of HelpBarTable (GetHelpBarData(row)), each an array of HelpBarData
 * with empty slots. */
#define OPTIMIZE_ROW 40000074       /* the set grid's Optimize guide, on no other screen seen */
#define KEY_L2 9                    /* HelpBarData's key code and button type */
#define GUIDE_PRIORITY 4            /* after Optimize, the last of the screen's three */

typedef struct { float x, y; } Vec2;

static unsigned char *module;       /* Il2CppUserAssemblies in the image */
#define GUEST(site) ((void *)(module+code_at[site].offset))
static unsigned char *set_text;         /* LocalizedTextBase<T>::SetText(this, term, method) */
static unsigned char *text_retranslate; /* LocalizedTextBase<T>::SetTextByTermInfo(this, method), any T */
static struct {
    char name[4*MAX_NAME+1];        /* UTF-8; empty: no name */
    unsigned char *shown, *term;    /* guest strings: the name (NULL after a change), its term */
} sets[MAX_SETS];
static unsigned char *screen;       /* PartyEditPartySetPartsMain while its set grid has the focus */
static unsigned char *label;        /* the number's text, while the screen is up */
static unsigned char *name_text;    /* the copy of it that shows the name */
static void *label_method;          /* the term setter's MethodInfo, passed as the game does */
static int current=-1;              /* the set shown */
static int loaded, l2_held;
/* The naming window: open from L2 until its close event. */
static struct {
    int open, typing;
    int answer;                     /* -1 none yet, 0 cancel, 1 accept */
    unsigned char *text;            /* the window's message text, once translated */
    void *method;                   /* the MethodInfo to translate it again with */
    unsigned char *shown[2];        /* its guest strings, in turn: the same one would not redraw */
    int flip;
    char typed[512];
} naming;

static void put32(void *p, int32_t v) { memcpy(p,&v,4); }

/* A name as kept: no surrounding blanks or control characters, at most MAX_NAME code points. */
static void clean_name(char *out, const char *text) {
    int points=0;
    size_t n=0;
    while (*text==' ') ++text;
    for (const unsigned char *p=(const unsigned char *)text; *p; ++p) {
        if (*p<0x20 || *p==0x7f) continue;
        if ((*p&0xC0)!=0x80 && ++points>MAX_NAME) break;
        out[n++]=(char)*p;
    }
    while (n && out[n-1]==' ') --n;
    out[n]=0;
}

static void names_path(char *path, size_t size) { snprintf(path,size,"%s/party_names.txt",runtime_file_user_dir()); }
static void load_names(void) {
    char path[600], line[256];
    names_path(path,sizeof(path));
    FILE *f=fopen(path,"rb");
    if (!f) return;
    while (fgets(line,sizeof(line),f)) {
        char *tab=strchr(line,'\t');
        const int number=atoi(line);
        if (!tab || number<1 || number>MAX_SETS) continue;
        tab[strcspn(tab,"\r\n")]=0;
        clean_name(sets[number-1].name,tab+1);
    }
    fclose(f);
}
static void save_names(void) {
    char path[600];
    names_path(path,sizeof(path));
    FILE *f=fopen(path,"wb");
    if (!f) { printf("Party names: cannot write %s\n",path); return; }
    for (int i=0;i<MAX_SETS;++i) if (sets[i].name[0]) fprintf(f,"%d\t%s\n",i+1,sets[i].name);
    fclose(f);
}

/* Writes UTF-8 `text` to the guest string `s` of room for `capacity` units. */
static void fill_string(unsigned char *s, const char *text, uint32_t capacity) {
    uint16_t *wide=(uint16_t *)(s+0x14);
    runtime_utf8_to_utf16(text,wide,capacity);
    int32_t length=0;
    while (wide[length]) ++length;
    put32(s+0x10,length);
}
/* A guest System.String of the class of `like`, in low memory, which the collector leaves alone. */
static unsigned char *new_string(const void *like, const char *text, uint32_t capacity) {
    unsigned char *s=runtime_low_alloc(0x16+2*(size_t)capacity);
    if (!s) return NULL;
    memcpy(s,like,8);
    fill_string(s,text,capacity);
    return s;
}

/* The name above the banner, through the game's term setter (translation below). */
static void show_name(const void *like) {
    if (!name_text || current<0) return;
    if (!sets[current].term) {
        char term[sizeof(NAME_TERM)+8];
        snprintf(term,sizeof(term),"%s%d",NAME_TERM,current+1);
        sets[current].term=new_string(like,term,sizeof(term));
    }
    if (sets[current].term)
        ((void (ABI *)(void *, void *, void *))set_text)(name_text,sets[current].term,label_method);
}

/* The copy of the number's text that shows the name: under the banner (Set_Ribbon), centred on the
 * screen, so that a box stretched over it and centred text are too. The number's parents up to there
 * (its box, then Content) lay out "Set" and the number in a row, and would take the copy into it. */
static void make_name_text(unsigned char *number) {
    void *from=((void *(ABI *)(void *, void *))GUEST(COMPONENT_TRANSFORM))(number,NULL);
    void *box=from ? ((void *(ABI *)(void *, void *))GUEST(TRANSFORM_PARENT))(from,NULL) : NULL;
    void *content=box ? ((void *(ABI *)(void *, void *))GUEST(TRANSFORM_PARENT))(box,NULL) : NULL;
    void *banner=content ? ((void *(ABI *)(void *, void *))GUEST(TRANSFORM_PARENT))(content,NULL) : NULL;
    if (!banner) return;
    name_text=((unsigned char *(ABI *)(void *, void *, int))GUEST(CLONE_WITH_PARENT))(number,banner,0);
    if (!name_text) return;
    const int size=((int (ABI *)(void *, void *))GUEST(TEXT_GET_FONT_SIZE))(number,NULL);
    ((void (ABI *)(void *, int, void *))GUEST(TEXT_SET_FONT_SIZE))(name_text,size*NAME_FONT_PERCENT/100,NULL);
    ((void (ABI *)(void *, int, void *))GUEST(TEXT_SET_ALIGNMENT))(name_text,ANCHOR_MIDDLE_CENTER,NULL);
    ((void (ABI *)(void *, int, void *))GUEST(TEXT_SET_H_OVERFLOW))(name_text,WRAP_OVERFLOW,NULL);
    ((void (ABI *)(void *, int, void *))GUEST(TEXT_SET_V_OVERFLOW))(name_text,WRAP_OVERFLOW,NULL);
    void *rect=((void *(ABI *)(void *, void *))GUEST(COMPONENT_TRANSFORM))(name_text,NULL);
    if (!rect) return;
    Vec2 size_delta=((Vec2 (ABI *)(void *, void *))GUEST(RECT_GET_SIZE))(rect,NULL);
    size_delta.x=NAME_WIDTH;
    ((void (ABI *)(void *, Vec2, void *))GUEST(RECT_SET_SIZE))(rect,size_delta,NULL);
    /* The box grows to the right of its pivot, so half its width back puts its middle on the banner's. */
    ((void (ABI *)(void *, Vec2, void *))GUEST(RECT_SET_ANCHORED))(rect,(Vec2){-NAME_WIDTH/2,NAME_ABOVE},NULL);
}

/* The call in UpdatePartySet: SetText(label, "Common/SetNo/<N>", method). The set's number is in the
 * term. */
static ABI int label_set(HookRegs *r) {
    static const char prefix[]="Common/SetNo/";
    const unsigned char *term=(const unsigned char *)r->rsi;
    if (!loaded) { loaded=1; load_names(); }
    int32_t length;
    memcpy(&length,term+0x10,4);
    int number=0;
    for (int32_t i=0;i<length;++i) {
        uint16_t c;
        memcpy(&c,term+0x14+2*i,2);
        if (i<(int)sizeof(prefix)-1) {
            if (c!=(unsigned char)prefix[i]) return 0;
        } else {
            if (c<'0' || c>'9' || number>MAX_SETS) return 0;
            number=number*10+(c-'0');
        }
    }
    if (number<1 || number>MAX_SETS) return 0;
    if (label!=(unsigned char *)r->rdi) {
        label=(unsigned char *)r->rdi;
        label_method=(void *)r->rdx;
        make_name_text(label);
    }
    current=number-1;
    show_name(term);
    return 0; /* the game's call goes on as it was */
}

/* The window's message for what has been typed. */
static void message(char *out, size_t size) {
    snprintf(out,size,"Name for Set %d\n\n%.*s_",current+1,4*MAX_NAME,naming.typed);
}
/* Called by the translation hook with `term`, and the text it is for: a set's name, the bar's guide, or
 * while the naming window is up, its message. Returns a guest string, or NULL for the game's own
 * translation. */
void *runtime_party_names_translation(void *text, void *method, const char *term, const void *like) {
    if (!strcmp(term,GUIDE_TERM)) {
        static unsigned char *guide;
        if (!guide) guide=new_string(like,"Rename set",10);
        return guide;
    }
    if (!strncmp(term,NAME_TERM,sizeof(NAME_TERM)-1)) {
        const int number=atoi(term+sizeof(NAME_TERM)-1);
        if (number<1 || number>MAX_SETS) return NULL;
        if (!sets[number-1].shown) sets[number-1].shown=new_string(like,sets[number-1].name,2*MAX_NAME);
        return sets[number-1].shown;
    }
    if (!naming.open) return NULL;
    if (!strcmp(term,"Common/Button/Yes") || !strcmp(term,"Common/Button/No")) {
        static unsigned char *ok, *cancel;
        if (!ok) ok=new_string(like,"OK",2);
        if (!cancel) cancel=new_string(like,"Cancel",6);
        return term[14]=='Y' ? ok : cancel;
    }
    if (strcmp(term,WINDOW_TERM)) return NULL;
    enum { CAPACITY=64+2*MAX_NAME };
    char shown[64+4*MAX_NAME];
    for (int i=0;i<2;++i) if (!naming.shown[i]) naming.shown[i]=new_string(like,"",CAPACITY);
    if (!naming.shown[0] || !naming.shown[1]) return NULL;
    if (text) { naming.text=(unsigned char *)text; naming.method=method; }
    message(shown,sizeof(shown));
    naming.flip^=1;
    fill_string(naming.shown[naming.flip],shown,CAPACITY);
    return naming.shown[naming.flip];
}
/* The message again, after typing: the text translates its term again, and sets the answer above in
 * whichever way its class has (this window's isn't a uGUI Text). */
static void update_message(void) {
    if (naming.text) ((void (ABI *)(void *, void *))text_retranslate)(naming.text,naming.method);
}

static void stop_typing(void) {
    if (naming.typing) theatrhythm_gpu_text_input_end();
    naming.typing=0;
    runtime_pad_text_mode(0);
}

/* L2 on the set grid: the game's window, with typing from here on. */
static void open_naming(void) {
    /* What the window's opener takes is only passed on to its close event, which reads the screen
     * from it: <>c__DisplayClass32_0's field at 0x10. */
    static unsigned char *stand_in;
    if (!stand_in) stand_in=runtime_low_alloc(0x20);
    if (!stand_in) return;
    memcpy(stand_in+0x10,&screen,8);
    const char *preset=getenv("THEATRHYTHM_IME_TEXT"); /* scripted runs, as the system dialog's */
    snprintf(naming.typed,sizeof(naming.typed),"%s",preset ? preset : sets[current].name);
    naming.open=1; naming.answer=-1; naming.text=NULL;
    naming.typing=theatrhythm_gpu_text_input_begin_silent(naming.typed);
    runtime_pad_text_mode(1);
    ((void (ABI *)(void *))GUEST(OPEN_WINDOW))(stand_in);
}

/* Layer::Update, on the game's main thread, every layer every frame. */
static ABI int layer_update(HookRegs *r) {
    (void)r;
    if (naming.open) {
        if (!naming.typing || naming.answer>=0) return 0;
        char typed[sizeof(naming.typed)];
        const int state=theatrhythm_gpu_text_input_poll(typed,sizeof(typed));
        if (strcmp(typed,naming.typed)) {
            memcpy(naming.typed,typed,sizeof(typed));
            update_message();
        }
        if (state) { /* Enter or Esc: the answer is known, and a press of the window's button closes it */
            naming.answer=state==1;
            runtime_pad_tap();
        }
        return 0;
    }
    if (!label || !screen || current<0) { l2_held=0; return 0; }
    const int l2=(runtime_pad_held()&L2_BUTTON)!=0;
    if (l2 && !l2_held && !theatrhythm_gpu_overlay_captures_input()) open_naming();
    l2_held=l2;
    return 0;
}

/* HelpBar::Draw(this, id): the guides it is about to show include the rows in its list (at 0xc0). With
 * the set grid in focus, the Optimize row gets a guide for L2 in an empty slot, which leaves it again
 * when anything else draws that row. The slots hold key (0x10), type (0x14), term (0x18), direction
 * (0x20) and priority (0x24); Draw skips a slot without a term. The term is looked for afresh every
 * time, as the table's data is the game's to replace. */
static ABI int help_bar_draw(HookRegs *r) {
    static unsigned char *term;
    unsigned char *list, *rows;
    memcpy(&list,(const char *)r->rdi+0xc0,8);
    if (!list) return 0;
    int32_t count;
    memcpy(&rows,list+0x10,8);
    memcpy(&count,list+0x18,4);
    for (int32_t i=0;i<count;++i) {
        unsigned char *row, *data;
        int32_t id, slots;
        memcpy(&row,rows+0x20+8*(size_t)i,8);
        memcpy(&id,row+0x10,4);
        if (id!=OPTIMIZE_ROW) continue;
        data=((unsigned char *(ABI *)(void *))GUEST(GET_HELP_DATA))(row);
        if (!data) return 0;
        memcpy(&slots,data+0x18,4);
        unsigned char *free_slot=NULL, *ours=NULL, *string=NULL;
        for (int32_t s=0;s<slots;++s) {
            unsigned char *slot, *text;
            int32_t length=0;
            memcpy(&slot,data+0x20+8*(size_t)s,8);
            if (!slot) continue;
            memcpy(&text,slot+0x18,8);
            if (text) { string=text; memcpy(&length,text+0x10,4); }
            if (text && text==term) ours=slot;
            if (!length && !free_slot) free_slot=slot;
        }
        const void *none=NULL;
        if (!screen) { if (ours) memcpy(ours+0x18,&none,8); return 0; }
        if (ours || !free_slot || !string) return 0;
        if (!term) term=new_string(string,GUIDE_TERM,sizeof(GUIDE_TERM));
        if (!term) return 0;
        put32(free_slot+0x10,KEY_L2);
        put32(free_slot+0x14,KEY_L2);
        memcpy(free_slot+0x18,&term,8);
        put32(free_slot+0x20,0);
        put32(free_slot+0x24,GUIDE_PRIORITY);
        return 0;
    }
    return 0;
}

/* The window's answer, <>c__DisplayClass32_1::<OnExecutedScenePartsMain>b__2(this, sender, data): the
 * button's number in data+0x18, the first being Yes. Left unset, the close event runs no Optimize. */
static ABI int window_answer(HookRegs *r) {
    if (!naming.open) return 0;
    if (naming.answer<0) {
        int32_t button;
        memcpy(&button,(const char *)r->rdx+0x18,4);
        naming.answer=button==0;
    }
    return 1;
}
/* Its events, ...b__3(this, event): 3 when closed, and the screen has the input again. */
static ABI int window_event(HookRegs *r) {
    if (!naming.open || (int32_t)r->rsi!=3) return 0;
    if (naming.typing && naming.answer!=0) { /* a controller's OK: what was typed until now */
        char typed[sizeof(naming.typed)];
        theatrhythm_gpu_text_input_poll(typed,sizeof(typed));
        memcpy(naming.typed,typed,sizeof(typed));
    }
    stop_typing();
    if (naming.answer==1 && current>=0) {
        clean_name(sets[current].name,naming.typed);
        sets[current].shown=NULL;
        save_names();
        if (naming.shown[0]) show_name(naming.shown[0]);
    }
    naming.open=0;
    naming.answer=-1;
    return 0;
}

static ABI int grid_focused(HookRegs *r) { screen=(unsigned char *)r->rdi; return 0; }
static ABI int grid_unfocused(HookRegs *r) { (void)r; screen=NULL; return 0; }
/* PartyEditMain::End: the screen is going. Its objects stay for the next visit, which makes a new copy
 * of the number's text, so this one goes. */
static ABI int party_edit_end(HookRegs *r) {
    (void)r;
    stop_typing();
    if (name_text && ((int (ABI *)(void *, void *))GUEST(OBJECT_ALIVE))(name_text,NULL)) {
        void *object=((void *(ABI *)(void *, void *))GUEST(COMPONENT_GAME_OBJECT))(name_text,NULL);
        if (object) ((void (ABI *)(void *, void *))GUEST(OBJECT_DESTROY))(object,NULL);
    }
    memset(&naming,0,sizeof(naming));
    screen=label=name_text=NULL; current=-1;
    return 0;
}

static const struct { int site; HookFn fn; } hooks[]={
    {H_LAYER_UPDATE,layer_update},
    {H_HELP_DRAW,help_bar_draw},
    {H_END,party_edit_end},
    {H_ANSWER,window_answer},
    {H_EVENT,window_event},
    {H_GRID_SET_FOCUS,grid_focused},
    {H_GRID_LOST_FOCUS,grid_focused},
    {H_GRID_REMOVE_FOCUS,grid_unfocused},
};

int runtime_party_names_install(unsigned char *image, uint64_t module_base, unsigned char *stubs, size_t size) {
    const size_t count=sizeof(hooks)/sizeof(*hooks);
    module=image+module_base;
    if (runtime_game_code_find(sites,SITES,code_at,"Party names") || (count+1)*runtime_hook_stub_size(0)>size) {
        printf("Party names: the Party Edit screen stays as it is\n");
        return -1;
    }
    unsigned char *site=GUEST(S_LABEL);
    site+=LABEL_CALL;
    int32_t rel;
    memcpy(&rel,site+1,4);
    set_text=site+5+rel;
    text_retranslate=(unsigned char *)GUEST(TEXT_RETRANSLATE_SITE)-RETRANSLATE_MOVED;
    unsigned char *p=stubs;
    for (size_t i=0;i<count;++i) {
        unsigned char *function=GUEST(hooks[i].site), *stub=p;
        const unsigned moved=code_at[hooks[i].site].moved;
        p=runtime_hook_stub(p,hooks[i].fn,0,function,moved);
        rel=(int32_t)(stub-(function+5));
        function[0]=0xe9; memcpy(function+1,&rel,4);
        memset(function+5,0x90,moved-5u); /* never run: the stub jumps past them */
    }
    /* The call site: the stub runs label_set, then goes on into the setter. */
    rel=(int32_t)(p-(site+5));
    runtime_hook_stub(p,label_set,0,set_text,0);
    memcpy(site+1,&rel,4);
    printf("Party names: L2 on the Party Edit screen names the shown set\n");
    return 0;
}
