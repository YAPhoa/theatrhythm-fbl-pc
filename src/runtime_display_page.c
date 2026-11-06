/* Display Settings in the game's own Options menu: a copy of the
 * Logo Usage button below Streaming Rules opens the game's MS Settings page, and while that page is
 * shown this way its rows show and change the runner's display settings instead of the game's. The
 * Language page's Display language row also steps through all nine languages the game has.
 *
 * Game functions (Il2CppUserAssemblies) are hooked at their entry: the first instructions move to a
 * stub that saves the argument registers, calls a runner function with them, and then either returns
 * a value for the game function or runs the moved instructions and goes on into it. Every site is
 * found by name and checked by its bytes first (runtime_game_code.c): a game version where one is
 * missing keeps its own pages, and Display Settings stays on F10. */
#include "runtime.h"
#include "gpu/theatrhythm_gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The registers a hook sees: the six argument registers (changes are passed on), and what the
 * game function returns when the hook returns 1. */
typedef struct { uint64_t rdi, rsi, rdx, rcx, r8, r9, rax, rdx_out; } HookRegs;
typedef int (ABI *HookFn)(HookRegs *regs);
typedef struct {
    int site;                    /* the function (sites: its moved prologue bytes, no rip-relative operands) */
    HookFn fn;
    unsigned char floats;        /* xmm argument registers the game function takes (kept for it) */
} Hook;

static unsigned char *module;    /* Il2CppUserAssemblies in the image */
static volatile int display_mode; /* the MS Settings page shows the display settings */

static uint64_t load(const void *p) { uint64_t v; memcpy(&v,p,8); return v; }
static int32_t load32(const void *p) { int32_t v; memcpy(&v,p,4); return v; }
static void put(void *p, uint64_t v) { memcpy(p,&v,8); }
static void put32(void *p, int32_t v) { memcpy(p,&v,4); }

/* The game code used here (runtime_game_code.c): hooked functions (H_), byte patches (P_) and
 * functions called (C_). Patterns from tools/game_code.py; a second one where 1.04 differs. */
enum {
    H_SWITCH_LAYER, H_TRANSLATE, H_SET_TEXT_A, H_SET_TEXT_B, H_LANGUAGE_CHANGED_A, H_LANGUAGE_CHANGED_B,
    H_BUTTON_VALUE, H_BUTTON_SET_INDEX, H_SLIDER_VALUE, H_SLIDER_SET_INDEX, H_INITIALIZE_LIST,
    H_INITIALIZE_SETTING, H_FILTER_ITEM, H_COMPARE_ITEM, H_SUBMIT, H_SLIDER_HORIZONTAL, H_SELECTOR_HORIZONTAL,
    H_CHECK_DEFAULT, H_TOP_SWITCH_IN, H_TOP_EXECUTED, H_TOP_SELECT, H_OBJECT_INFO, H_ROW_VIEW, H_HELP_ENTRY,
    H_LANGUAGE_INPUT,
    P_REGION_CURRENT, P_REGION_WORLD, P_REGION_JAPAN, P_SET_DISPLAY_LANGUAGE, P_DESERIALIZE, P_PICKER,
    C_REFRESH_VIEW, C_PLAY_SE, C_LANGUAGE_CONTROLLER, C_LANGUAGE_CHANGED, C_GET_TRANSFORM, C_GET_PARENT,
    C_CHILD_COUNT, C_GET_CHILD, C_GET_NAME, C_GET_GAME_OBJECT, C_GET_COMPONENT, C_SPRITE_PACKED,
    C_SPRITE_PACKING_MODE, C_SPRITE_TEXTURE, C_SPRITE_TEXTURE_RECT, C_SPRITE_RECT, C_SPRITE_RECT_OFFSET,
    C_SPRITE_PIVOT, C_SPRITE_PIXELS_PER_UNIT, C_CREATE_SPRITE, C_SET_SPRITE, C_CLONE, C_SET_NAME, C_SET_ALPHA,
    C_GET_TYPE, C_COMPONENT_IN_CHILDREN, C_PARENT_SCENE_PARTS, C_GET_POSITION, C_SET_POSITION, C_REGISTER,
    C_GET_LOCAL_POSITION, C_SET_LOCAL_POSITION, C_RECT, C_SET_ACTIVE, C_ON_INITIALIZE_SETTING,
    C_FONT_RESOURCES, C_SWITCH_LANGUAGE, C_CONFIG_INFO, C_RESERVE_SAVE, C_END_ACTION, SITES
};
/* LocalizedTextBase<T> (shared generic code, two instances; no method of its own). */
#define SET_TEXT_BY_TERM "55 48 89 e5 41 56 53 | 48 8b 46 20 48 89 f3 49 89 fe 48 8b 80 c0 00 00 00 48 8b 70 78 ff 56 08 " \
    "48 85 c0 74 3d 48 8b 43 20 4c 89 f7 48 8b 80 c0 00 00 00 48 8b 70 78 ff 56 08 48 89 c7 31 f6 e8 ?? ?? ?? ??"
#define ON_LANGUAGE_CHANGED "55 48 89 e5 41 57 41 56 | 53 50 48 8b 41 20 49 89 cf 41 89 d6 48 89 fb 48 8b 80 c0 00 00 00 " \
    "48 8b 70 08 ff 56 08 44 39 f0 75 0b 48 83 c4 08 5b 41 5e 41 5f 5d c3 48 8b 0d ?? ?? ?? ?? 48 85 c9 75 1b"
#define REGION_LANGUAGE "83 ff 0f 7e 1a 83 ff 22 77 2b" /* cmp edi,15; jle; cmp edi,34; ja (the World region check) */
static const GameSite sites[SITES]={
    {"Game.Scene.Config.ConfigMain::switchLayer", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 53 48 83 e4 e0 48 83 ec 60 4c 8b 3d ?? ?? ?? ??"}},
    {"I2.Loc.LocalizationManager::TryGetTranslation", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 48 83 ec 28 80 3d ?? ?? ?? ?? 00"}},
    {NULL, 0, 2, 0, {SET_TEXT_BY_TERM}},
    {NULL, 0, 2, 1, {SET_TEXT_BY_TERM}},
    {NULL, 0, 2, 0, {ON_LANGUAGE_CHANGED}},
    {NULL, 0, 2, 1, {ON_LANGUAGE_CHANGED}},
    {"Game.Scene.Config.ConfigButtonBehaviour::refreshValueText", 0, 0, 0,
     {"55 48 89 e5 41 56 53 | 80 3d ?? ?? ?? ?? 00 48 89 fb 75 13 48 8d 3d ?? ?? ?? ??"}},
    {"Game.Scene.Config.ConfigButtonBehaviour::SetIndex", 0, 0, 0,
     {"55 48 89 e5 41 56 53 | 89 77 18 48 89 fb 80 3d ?? ?? ?? ?? 00 75 16 48 8d 3d ?? ?? ?? ??"}},
    {"Game.Scene.Config.ConfigSliderBehaviour::refreshValueText", 0, 0, 0,
     {"55 48 89 e5 41 56 53 | 80 3d ?? ?? ?? ?? 00 48 89 fb 75 13 48 8d 3d ?? ?? ?? ??",
      "55 48 89 e5 41 57 41 56 | 53 50 80 3d ?? ?? ?? ?? 00 48 89 fb 75 1f 48 8d 3d ?? ?? ?? ??"}},
    {"Game.Scene.Config.ConfigSliderBehaviour::SetIndex", 0, 0, 0,
     {"55 48 89 e5 41 56 53 | 89 77 18 48 89 fb c5 fa 2a c6 48 8b 7f 20 48 8b 07",
      "55 48 89 e5 53 50 | 89 77 18 48 89 fb c5 fa 2a c6 48 8b 7f 20 48 8b 07 48 8b b0 30 04 00 00"}},
    {"Game.Scene.Config.ConfigMusicStageEditPartsMain::InitializeList", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 48 83 ec 18 80 3d ?? ?? ?? ?? 00"}},
    {"Game.Scene.Config.ConfigListItemBase::InitializeSetting", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 54 53 80 3d ?? ?? ?? ?? 00 49 89 fe 75 2b 48 8d 3d ?? ?? ?? ??"}},
    {"Game.Scene.Config.ConfigMusicStageEditPartsMain::filterItem", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 53 50 80 3d ?? ?? ?? ?? 00 49 89 f7 49 89 fe 75 43"}},
    {"Game.Scene.Config.ConfigMusicStageEditPartsMain::compareItem", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 53 50 80 3d ?? ?? ?? ?? 00 48 89 d3 49 89 f6 75 13"}},
    {"Game.Scene.Config.ConfigListItemButton::OnSubmit", 0, 0, 0,
     {"55 48 89 e5 53 50 | 48 89 fb 48 8b 7f 58 48 85 ff 74 13 83 7f 10 00 74 0d"}},
    {"Game.Scene.Config.ConfigListItemSlider::OnHorizontalInput", 0, 0, 0,
     {"55 48 89 e5 41 56 53 | 48 8b 4f 38 49 89 fe 83 fe 01 8b 41 14 74 11 85 f6"}},
    {"Game.Scene.Config.ConfigListItemSelector::OnHorizontalInput", 0, 0, 0,
     {"55 48 89 e5 41 56 53 | 48 8b 4f 38 89 f0 48 89 fb 83 f8 01 8b 71 14 74 10"}},
    {"Game.Scene.Config.ConfigMusicStageEditPartsMain::openCheckDefaultWindow", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 48 83 ec 18 80 3d ?? ?? ?? ?? 00"}},
    {"Game.Scene.Config.ConfigTopLayerMain::Game.ISwitchLayer.SwitchIn", 0, 0, 0,
     {"55 48 89 e5 41 56 53 | 48 83 e4 e0 48 83 ec 40 4c 8b 35 ?? ?? ?? ?? 48 89 fb"}},
    {"Game.Scene.Config.ConfigTopPartsMain::OnExecutedScenePartsMain", 0, 0, 0,
     {"55 48 89 e5 41 56 53 | 80 3d ?? ?? ?? ?? 00 48 89 f3 75 2b 48 8d 3d ?? ?? ?? ??"}},
    {"Game.Scene.Config.ConfigTopPartsMain::sendSelectReaction", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 48 83 ec 18 48 8b 05 ?? ?? ?? ??"}},
    {"Game.ScenePartsMain/UIObjectGroup::GetUIObjectInfo", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 50 80 3d ?? ?? ?? ?? 00 48 89 f3"}},
    {"Game.Scene.Config.ConfigListItemView::OnSetView", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 50 80 3d ?? ?? ?? ?? 00 49 89 f6"}},
    {"Game.Scene.Common.CommonMenu.HelpBarContent::RefleshDisplay", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 48 83 ec 68 80 3d ?? ?? ?? ?? 00"}},
    {"Game.Scene.Config.ConfigLanguageMenuEditPartsMain::OnExecutedScenePartsMain", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 54 53 80 3d ?? ?? ?? ?? 00 49 89 f4 48 89 fb"}},
    {"Game.Localization.RegionLanguage::GetLanguageAllowedInCurrentRegion", 0, 0, 0, {REGION_LANGUAGE}},
    {"Game.Localization.RegionLanguage::getLanguageAllowedInWorldRegion", 0, 0, 0, {REGION_LANGUAGE}},
    {"Game.Localization.RegionLanguage::getLanguageAllowedInJapanRegion", 0, 0, 0, {REGION_LANGUAGE}},
    {"Game.Backup.ConfigInfo::set_DisplayLanguage", 0, 0, 0, {"83 fe 0f 7e 1d 83 fe 22 77 2e"}},
    /* the DisplayLanguage field's World region check, up to the store its patch jumps to */
    {"Game.MessagePackFormatter.Formatters.Game.Backup.ConfigInfoFormatter::Deserialize", 1, 0, 0,
     {"83 f8 0f 7e 1b 83 f8 22 77 2c 89 c1 48 ba 00 00 60 00 04 00 00 00 48 0f a3 ca 89 c1 72 1d eb 16 "
      "b9 0a 00 00 00 83 f8 0a 74 11 89 c1 83 e1 fe 83 f9 0e 89 c1 74 05 b9 0a 00 00 00"}},
    {"Game.Scene.WindowLanguageSetting.WindowLanguageSettingBodyPartsMain/<InitializeSetting>d__5::MoveNext", 1, 0, 0,
     {"44 8b 60 18 eb 41"}},
    {"Gummi.Scene.UI.UIListItem::RefreshView", 0, 0, 0, {""}},
    {"Gummi.Sound.SoundManager::PlaySE", 0, 0, 0, {""}},
    {"Gummi.Localization.LanguageController::get_Instance", 0, 0, 0, {""}},
    {"Gummi.Localization.LanguageController/LanguageChangedEventHandler::Invoke", 0, 0, 0, {""}},
    {"UnityEngine.Component::get_transform", 0, 0, 0, {""}},
    {"UnityEngine.Transform::get_parent", 0, 0, 0, {""}},
    {"UnityEngine.Transform::get_childCount", 0, 0, 0, {""}},
    {"UnityEngine.Transform::GetChild", 0, 0, 0, {""}},
    {"UnityEngine.Object::get_name", 0, 0, 0, {""}},
    {"UnityEngine.Component::get_gameObject", 0, 0, 0, {""}},
    {"UnityEngine.GameObject::GetComponentByName", 0, 0, 0, {""}},
    {"UnityEngine.Sprite::get_packed", 0, 0, 0, {""}},
    {"UnityEngine.Sprite::get_packingMode", 0, 0, 0, {""}},
    {"UnityEngine.Sprite::get_texture", 0, 0, 0, {""}},
    {"UnityEngine.Sprite::get_textureRect", 0, 0, 0, {""}},
    {"UnityEngine.Sprite::get_rect", 0, 0, 0, {""}},
    {"UnityEngine.Sprite::get_textureRectOffset", 0, 0, 0, {""}},
    {"UnityEngine.Sprite::get_pivot", 0, 0, 0, {""}},
    {"UnityEngine.Sprite::get_pixelsPerUnit", 0, 0, 0, {""}},
    {"UnityEngine.Sprite::CreateSprite", 0, 0, 0, {""}},
    {"UnityEngine.UI.Image::set_sprite", 0, 0, 0, {""}},
    {"UnityEngine.Object::Internal_CloneSingleWithParent", 0, 0, 0, {""}},
    {"UnityEngine.Object::set_name", 0, 0, 0, {""}},
    {"UnityEngine.CanvasGroup::set_alpha", 0, 0, 0, {""}},
    {"System.Object::GetType", 0, 0, 0, {""}},
    {"UnityEngine.Component::GetComponentInChildren", 0, 0, 0, {""}},
    {"Gummi.Scene.UI.UIObject::get_ParentUISceneParts", 0, 0, 0, {""}},
    {"UnityEngine.Transform::get_position", 0, 0, 0, {""}},
    {"UnityEngine.Transform::set_position", 0, 0, 0, {""}},
    {"Gummi.Scene.UI.UISceneParts::Gummi.Scene.UI.IUIObjectRegister.Register", 0, 0, 0, {""}},
    {"UnityEngine.Transform::get_localPosition", 0, 0, 0, {""}},
    {"UnityEngine.Transform::set_localPosition", 0, 0, 0, {""}},
    {"UnityEngine.RectTransform::get_rect", 0, 0, 0, {""}},
    {"UnityEngine.GameObject::SetActive", 0, 0, 0, {""}},
    {"Game.Scene.Config.ConfigMusicStageEditPartsMain::OnInitializeSetting", 0, 0, 0, {""}},
    {"Game.Localization.FontResourceManager::get_Instance", 0, 0, 0, {""}},
    {"Gummi.Localization.LanguageController::SwitchLanguage", 0, 0, 0, {""}},
    {"Game.Backup.Backup::get_ConfigInfo", 0, 0, 0, {""}},
    {"Game.Backup.Backup::ReserveSave", 0, 0, 0, {""}},
    {"Gummi.Scene.UI.UIObject::EndAction", 0, 0, 0, {""}},
};
static GameCode code_at[SITES];
#define CODE(site) ((void *)(module+code_at[site].offset))

/* A guest System.String (length at +0x10, UTF-16 from +0x14) as ASCII, others as '?'. */
static void guest_string(const void *s, char *out, size_t size) {
    size_t n=0;
    if (s) {
        const int32_t length=load32((const char *)s+0x10);
        const uint16_t *chars=(const uint16_t *)((const char *)s+0x14);
        for (int32_t i=0;i<length && n+1<size;++i) out[n++]=chars[i]<128 ? (char)chars[i] : '?';
    }
    out[n]=0;
}
/* A guest System.String holding ASCII `text`, of the class of the guest string `like`: one per text,
 * kept in low memory outside the game's heap, so the collector never frees or moves it. */
static void *guest_text(const void *like, const char *text) {
    static struct { char text[160]; void *s; } made[96];
    static unsigned count;
    for (unsigned i=0;i<count;++i) if (!strcmp(made[i].text,text)) return made[i].s;
    const size_t n=strlen(text);
    if (count==sizeof(made)/sizeof(*made) || n>=sizeof(made[0].text)) return NULL;
    unsigned char *s=runtime_low_alloc(0x16+2*n);
    if (!s) return NULL;
    memcpy(s,like,8);
    put32(s+0x10,(int32_t)n);
    for (size_t i=0;i<n;++i) { const uint16_t c=(uint8_t)text[i]; memcpy(s+0x14+2*i,&c,2); }
    memcpy(made[count].text,text,n+1); made[count++].s=s;
    return s;
}
/* The class name of a guest object (Il2CppClass: image, gc_desc, name, namespace). */
static const char *class_name(const void *object) { return (const char *)load((const char *)load(object)+0x10); }

#define LOGO_LAYER 8           /* ActiveLayerType.LogoIllustration */
#define MUSIC_STAGE_LAYER 3    /* ActiveLayerType.MusicStage: the MS Settings page */

/* The page's rows that show display settings, found by their help text term. */
enum { ROW_WINDOW, ROW_RESOLUTION, ROW_VSYNC, ROW_FPS, ROW_RENDER, ROWS };
static const struct { const char *detail, *help; } rows[ROWS]={
    {"Common/DetailText/Config0018","Windowed, a borderless window covering the screen, or exclusive fullscreen."},
    {"Common/DetailText/Config0019","The window's size when windowed, the screen mode in fullscreen. Borderless always uses the desktop resolution."},
    {"Common/DetailText/Config0024","Waits for the monitor: no tearing, slightly more input delay."},
    {"Common/DetailText/Config0042","Caps how many frames are shown per second. Off follows the display."},
    /* Timing display's row, an On/Off selector like VSync's */
    {"Common/DetailText/Config0026","4K as on PS4 Pro, filtered down to the window: sharper. 1080p as on PS4: lighter on the GPU. Applies after a restart."},
};
static const char *const mode_names[]={"Windowed","Borderless windowed","Fullscreen"};
static const int fps_stops[]={0,30,60,120,144,240}; /* the slider's six stops */
static const char *const fps_ticks[]={"Off","30","60","120","144","240"};

static unsigned char *page;            /* ConfigMusicStageEditPartsMain of the loaded Config scene */
static unsigned char *row_item[ROWS];  /* its ConfigListItemBase rows */
static int page_display;               /* the rows the page lists: 1 display settings */
static int resorting;                  /* OnInitializeSetting called from here */
static int forcing;                    /* retranslate(): the rows' texts translate again */
static void *label_text;               /* LocalizedText being translated (SetTextByTermInfo) */
static int display_dirty;              /* settings.ini needs saving */
static struct { unsigned char *view; float x; int moved; } marked[2]; /* row views without their marks: label x, label moved */

/* A new Config scene: forget the rows of the last one. */
static void note_page(unsigned char *self) {
    if (self==page) return;
    page=self; page_display=0;
    memset(row_item,0,sizeof(row_item));
    memset(marked,0,sizeof(marked));
}
static int row_of(const unsigned char *item) {
    for (int i=0;i<ROWS;++i) if (item && item==row_item[i]) return i;
    return -1;
}

/* Guest calls (sysv, MethodInfo last: NULL as the game passes it). */
static void refresh_view(unsigned char *item) {
    ((void (ABI *)(void *, void *))CODE(C_REFRESH_VIEW))(item,NULL); /* UIListItem::RefreshView */
}
static void play_se(const void *label) { /* SoundManager::PlaySE(label, delay 0, loop false) */
    if (label && load32((const char *)label+0x10)>0)
        ((void (ABI *)(const void *, float, int, void *))CODE(C_PLAY_SE))(label,0.0f,0,NULL);
}
/* LanguageController.Instance.LanguageChanged(current, current) with `forcing` set: the texts in
 * the rows translate their terms again (on_language_changed), so the labels follow the mode, and
 * the rows refresh. */
static void retranslate(void) {
    unsigned char *controller=((unsigned char *(ABI *)(void *))CODE(C_LANGUAGE_CONTROLLER))(NULL);
    if (!controller) return;
    void *handler=(void *)load(controller+0x10);
    const int32_t language=load32(controller+0x18);
    forcing=1;
    if (handler) ((void (ABI *)(void *, int32_t, int32_t, void *))CODE(C_LANGUAGE_CHANGED))(handler,language,language,NULL);
    forcing=0;
}
static unsigned char *transform(void *component) { /* Component::get_transform */
    return ((unsigned char *(ABI *)(void *, void *))CODE(C_GET_TRANSFORM))(component,NULL);
}
static unsigned char *parent(void *t) { return ((unsigned char *(ABI *)(void *, void *))CODE(C_GET_PARENT))(t,NULL); }
static int32_t child_count(void *t) { return ((int32_t (ABI *)(void *, void *))CODE(C_CHILD_COUNT))(t,NULL); }
static unsigned char *child(void *t, int32_t i) { return ((unsigned char *(ABI *)(void *, int32_t, void *))CODE(C_GET_CHILD))(t,i,NULL); }
static void *object_name(void *object) { return ((void *(ABI *)(void *, void *))CODE(C_GET_NAME))(object,NULL); } /* Object::get_name */
/* The component of class `type` (a name) on the game object of `component`, or NULL. */
static unsigned char *component_named(void *component, const char *type) {
    void *go=((void *(ABI *)(void *, void *))CODE(C_GET_GAME_OBJECT))(component,NULL); /* Component::get_gameObject */
    void *name=guest_text(object_name(component),type);
    return go && name ? ((unsigned char *(ABI *)(void *, void *, void *))CODE(C_GET_COMPONENT))(go,name,NULL) : NULL; /* GetComponent(string) */
}
/* Two Unity objects with the same native object (m_CachedPtr). */
static int same(const unsigned char *a, const unsigned char *b) { return a && b && load(a+0x10) && load(a+0x10)==load(b+0x10); }
/* Whether transform `t` is `root` or below it. */
static int under(unsigned char *t, const unsigned char *root) {
    for (int depth=0;t && depth<12;++depth,t=parent(t)) if (same(t,root)) return 1;
    return 0;
}

/* Whether a component sits in one of the item's views: its transform or a parent is the view's. */
static int in_row(void *component, const unsigned char *item) {
    const unsigned char *views=item ? (const unsigned char *)load(item+0x10) : NULL; /* List<UIListItemView> */
    if (!views) return 0;
    const unsigned char *array=(const unsigned char *)load(views+0x10);
    const int32_t count=load32(views+0x18);
    unsigned char *t=transform(component);
    for (int32_t i=0;i<count;++i) {
        const unsigned char *view=(const unsigned char *)load(array+0x20+8*i);
        if (view && under(t,(const unsigned char *)load(view+0x18))) return 1; /* _rectTransform */
    }
    return 0;
}

/* The row of a component in display mode, or -1. */
static int row_holding(void *component) {
    for (int row=0;row<ROWS;++row) if (in_row(component,row_item[row])) return row;
    return -1;
}
/* The row whose view has this ConfigUIBehaviourBase (ConfigListItemView._uiBehaviour), or -1. */
static int row_of_behaviour(const void *behaviour) {
    for (int row=0;row<ROWS;++row) {
        const unsigned char *views=row_item[row] ? (const unsigned char *)load(row_item[row]+0x10) : NULL;
        if (!views) continue;
        const unsigned char *array=(const unsigned char *)load(views+0x10);
        for (int32_t i=0;i<load32(views+0x18);++i) {
            const unsigned char *view=(const unsigned char *)load(array+0x20+8*i);
            if (view && load(view+0x40)==(uint64_t)(uintptr_t)behaviour) return row;
        }
    }
    return -1;
}

/* The value a row shows: VSync, the frame rate limit and the render resolution (1080p left, 4K
 * right) as the selected index; the pills (Window mode, Resolution) draw theirs in button_value. */
static void show_row(int row) {
    unsigned char *item=row_item[row];
    if (!item) return;
    unsigned char *parameter=(unsigned char *)load(item+0x38); /* ConfigListItemView.ViewParameter */
    if (row==ROW_VSYNC) put32(parameter+0x14,theatrhythm_display_get(THEATRHYTHM_DISPLAY_VSYNC) ? 0 : 1);
    else if (row==ROW_RENDER) put32(parameter+0x14,theatrhythm_display_get(THEATRHYTHM_DISPLAY_RENDER_4K) ? 1 : 0);
    else if (row==ROW_FPS) {
        const int fps=theatrhythm_display_get(THEATRHYTHM_DISPLAY_FPS_LIMIT);
        int at=fps<0 ? 5 : 0; /* Unlimited (set in settings.ini or the F10 panel): the last stop */
        for (int i=1;fps>0 && i<6;++i) if (abs(fps_stops[i]-fps)<abs(fps_stops[at]-fps) || !at) at=i;
        put32(parameter+0x14,at);
    }
    refresh_view(item);
}

/* The Display Settings button: a copy of Logo Usage, put in the menu's empty slot below Streaming
 * Rules. It keeps Logo Usage's number (UIObject._value), so the menu runs it as that button (its
 * description, its page); the hooks tell it apart by its object. */
#define LOGO_BUTTON 7
#define STREAMING_BUTTON 5
static const char button_name[]="TheatrhythmDisplaySettings";
static unsigned char *button;          /* its UIObject in the loaded Config scene */
static unsigned char *button_root;     /* its RectTransform (the copied button) */
static unsigned char *logo_button;     /* Logo Usage's UIObject, which it stands in for */
static int button_pressed;             /* the logo layer opens for it, in display mode */
static int describing;                 /* the menu's description is the button's */
static int monitor_icon;               /* the Config atlas holds the monitor icon (install) */

typedef struct { float x, y; } Vec2;
typedef struct { float x, y, z; } Vec3;
typedef struct { float x, y, z, w; } Vec4;
typedef struct { float x, y, width, height; } Rect;

/* ConfigTopPartsMain's button `value`: its focusable UIObjects (ScenePartsMain._uIObjectGroupFocusable
 * .UIObjectList, of UIObjectInfo). */
static unsigned char *top_button(const unsigned char *parts, int value) {
    const unsigned char *group=(const unsigned char *)load(parts+0x88);
    const unsigned char *list=group ? (const unsigned char *)load(group+0x10) : NULL;
    if (!list) return NULL;
    const unsigned char *array=(const unsigned char *)load(list+0x10);
    for (int32_t i=0;i<load32(list+0x18);++i) {
        const unsigned char *info=(const unsigned char *)load(array+0x20+8*i);
        unsigned char *object=info ? (unsigned char *)load(info+0x10) : NULL;
        if (object && load32(object+0x20)==value) return object;
    }
    return NULL;
}
/* The menu's cursor moves: each button lists its neighbour per CursorDirection
 * (UIFocusableFixedMoveButton._movingUIObjects, UIObject[4]). */
enum { UP, DOWN, LEFT, RIGHT };
static unsigned char *moves(const unsigned char *button) {
    unsigned char *array=(unsigned char *)load(button+0x80);
    return array && load(array+0x18)>=4 ? array : NULL;
}
static unsigned char *move(const unsigned char *button, int direction) {
    unsigned char *array=moves(button);
    return array ? (unsigned char *)load(array+0x20+8*direction) : NULL;
}
static void set_move(const unsigned char *button, int direction, const void *to) {
    unsigned char *array=moves(button);
    if (array) put(array+0x20+8*direction,(uint64_t)(uintptr_t)to);
}
/* Link the Display Settings button (`copy`, whose moves are Logo Usage's) into the grid: the empty
 * slot below Streaming Rules and right of Logo Usage, the end of the reading order. */
static int link_button(const unsigned char *parts, unsigned char *copy) {
    unsigned char *logo=top_button(parts,LOGO_BUTTON), *streaming=top_button(parts,STREAMING_BUTTON);
    unsigned char *ms=top_button(parts,2), *first=top_button(parts,1);
    if (!moves(copy) || !moves(logo) || !moves(streaming) || !ms || !first) return 0;
    if (moves(copy)==moves(logo)) return 0; /* Instantiate copies arrays; never share Logo Usage's */
    set_move(copy,UP,streaming); set_move(copy,DOWN,ms); set_move(copy,LEFT,logo); set_move(copy,RIGHT,move(logo,RIGHT));
    set_move(logo,RIGHT,copy); set_move(streaming,DOWN,copy);
    if (move(ms,UP)==streaming) set_move(ms,UP,copy); /* the column's wrap */
    if (move(first,LEFT)==logo) set_move(first,LEFT,copy); /* the reading order's wrap */
    return 1;
}
/* The child of `container` that holds transform `t`. */
static unsigned char *child_holding(unsigned char *t, const unsigned char *container) {
    for (unsigned char *up;t;t=up) {
        up=parent(t);
        if (same(up,container)) return t;
    }
    return NULL;
}
/* The Image at or below transform `t` (a few levels down) that shows the sprite named `sprite`. */
static unsigned char *image_showing(unsigned char *t, const char *sprite, int depth) {
    unsigned char *image=component_named(t,"Image");
    const void *shown=image ? (const void *)load(image+0xd0) : NULL; /* Image.m_Sprite */
    char name[48];
    if (shown) { guest_string(object_name((void *)shown),name,sizeof(name)); if (!strcmp(name,sprite)) return image; }
    for (int32_t i=0;depth>0 && i<child_count(t);++i)
        if ((image=image_showing(child(t,i),sprite,depth-1))) return image;
    return NULL;
}
/* scripts/config_icon.py draws the monitor in an unused area of the Config atlas (PATCH, in blocks
 * counted from the bottom row) instead of over Logo Usage's icon (ICON_FROM), so the copy's icon is
 * Logo Usage's sprite moved by that many texels. */
#define ICON_MOVE_X ((208-52)*4)
#define ICON_MOVE_Y ((192-218)*4)
/* The copied button below `root` shows the monitor: a sprite like Logo Usage's (size, pivot, pixels
 * per unit) over the same atlas texture at the monitor's place, Sprite.CreateSprite(texture, rect,
 * pivot, pixelsPerUnit, extrude 0, FullRect, no border, no physics shape). The copy's Image holds
 * the only reference, so the sprite goes with the scene. */
static void show_monitor(unsigned char *root) {
    unsigned char *image=image_showing(root,"ConfigTop_Button_Icon_07",6);
    void *sprite=image ? (void *)load(image+0xd0) : NULL;
    if (!sprite) { printf("Display page: no Logo Usage icon in the copy; it keeps that icon\n"); return; }
    const int packed=((int (ABI *)(void *, void *))CODE(C_SPRITE_PACKED))(sprite,NULL)&0xff;     /* Sprite::get_packed */
    const int tight=packed && !((int (ABI *)(void *, void *))CODE(C_SPRITE_PACKING_MODE))(sprite,NULL); /* packingMode Tight */
    void *texture=((void *(ABI *)(void *, void *))CODE(C_SPRITE_TEXTURE))(sprite,NULL);             /* Sprite::get_texture */
    if (tight || !texture) { printf("Display page: Logo Usage's icon is not a rectangle in its atlas; the copy keeps it\n"); return; }
    /* textureRect: the drawn part in the atlas; it sits at textureRectOffset in the sprite's rect. */
    const Rect drawn=((Rect (ABI *)(void *, void *))CODE(C_SPRITE_TEXTURE_RECT))(sprite,NULL), whole=((Rect (ABI *)(void *, void *))CODE(C_SPRITE_RECT))(sprite,NULL);
    const Vec2 offset=((Vec2 (ABI *)(void *, void *))CODE(C_SPRITE_RECT_OFFSET))(sprite,NULL), pivot=((Vec2 (ABI *)(void *, void *))CODE(C_SPRITE_PIVOT))(sprite,NULL);
    const float units=((float (ABI *)(void *, void *))CODE(C_SPRITE_PIXELS_PER_UNIT))(sprite,NULL);
    if (whole.width<=0 || whole.height<=0) return;
    const Rect rect={drawn.x+ICON_MOVE_X-offset.x,drawn.y+ICON_MOVE_Y-offset.y,whole.width,whole.height};
    const Vec2 center={pivot.x/whole.width,pivot.y/whole.height};
    const Vec4 border={0,0,0,0};
    void *monitor=((void *(ABI *)(void *, Rect, Vec2, float, uint32_t, int32_t, Vec4, int, void *))CODE(C_CREATE_SPRITE))
        (texture,rect,center,units,0,0,border,0,NULL);
    if (!monitor) { printf("Display page: cannot make the monitor icon; the copy keeps Logo Usage's\n"); return; }
    ((void (ABI *)(void *, void *, void *))CODE(C_SET_SPRITE))(image,monitor,NULL); /* Image::set_sprite */
}

/* ConfigTopLayerMain::SwitchIn(this), as the Options menu shows: give its scene the Display Settings
 * button once. Logo Usage's button is copied into itself (Object.Instantiate(original, parent)), so the
 * menu's animations, which find their objects by name, fade the copy with it; the copy sits below
 * Streaming Rules, and its UIObject is registered with the buttons' UISceneParts, which also links it
 * into the cursor's moves. */
static ABI int top_switch_in(HookRegs *r) {
    const unsigned char *parts=(const unsigned char *)load((unsigned char *)r->rdi+0x48); /* _configTop */
    unsigned char *logo=parts ? top_button(parts,LOGO_BUTTON) : NULL, *streaming=parts ? top_button(parts,STREAMING_BUTTON) : NULL;
    if (!logo || !streaming) return 0;
    unsigned char *container=NULL;
    for (unsigned char *t=transform(logo);t && !container;t=parent(t)) if (under(transform(streaming),t)) container=t;
    unsigned char *logo_root=container ? child_holding(transform(logo),container) : NULL;
    unsigned char *streaming_root=container ? child_holding(transform(streaming),container) : NULL;
    if (!logo_root || !streaming_root) return 0;
    for (int32_t i=0;i<child_count(logo_root);++i) { /* this scene already has it */
        char name[48]; guest_string(object_name(child(logo_root,i)),name,sizeof(name));
        if (!strcmp(name,button_name)) return 0;
    }
    button=NULL; button_root=NULL;
    unsigned char *root=((unsigned char *(ABI *)(void *, void *, int))CODE(C_CLONE))(logo_root,logo_root,0); /* Internal_CloneSingleWithParent */
    if (!root) return 0;
    void *name=guest_text(object_name(logo_root),button_name);
    if (name) ((void (ABI *)(void *, void *, void *))CODE(C_SET_NAME))(root,name,NULL); /* Object::set_name */
    /* The button fades in through a CanvasGroup on its root, copied at alpha 0: the original's now
     * fades the copy too. */
    void *group=component_named(root,"CanvasGroup");
    if (group) ((void (ABI *)(void *, float, void *))CODE(C_SET_ALPHA))(group,1.0f,NULL); /* CanvasGroup::set_alpha */
    void *type=((void *(ABI *)(void *, void *))CODE(C_GET_TYPE))(logo,NULL); /* Object::GetType */
    unsigned char *copy=((unsigned char *(ABI *)(void *, void *, int, void *))CODE(C_COMPONENT_IN_CHILDREN))(root,type,1,NULL); /* GetComponentInChildren */
    void *scene_parts=((void *(ABI *)(void *, void *))CODE(C_PARENT_SCENE_PARTS))(logo,NULL); /* UIObject::get_ParentUISceneParts */
    if (!copy || !scene_parts) return 0;
    /* Below Streaming Rules, in Logo Usage's row. The buttons slide in from the left inside their
     * roots, which stay put, and the copy keeps the slide's first frame: its button goes where the
     * others come to rest, on their roots (world positions). */
    Vec3 (ABI *get_position)(void *, void *)=(Vec3 (ABI *)(void *, void *))CODE(C_GET_POSITION); /* Transform::position */
    Vec3 at=get_position(root,NULL);
    const Vec3 column=get_position(streaming_root,NULL), row=get_position(logo_root,NULL), now=get_position(transform(copy),NULL);
    at.x+=column.x-now.x; at.y+=row.y-now.y;
    ((void (ABI *)(void *, Vec3, void *))CODE(C_SET_POSITION))(root,at,NULL);
    if (monitor_icon) show_monitor(root);
    ((void (ABI *)(void *, void *, void *))CODE(C_REGISTER))(scene_parts,copy,NULL); /* UISceneParts IUIObjectRegister.Register */
    if (!link_button(parts,copy)) printf("Display page: the Display Settings button has no cursor moves\n");
    button=copy; button_root=root; logo_button=logo;
    printf("Display page: Display Settings button added below Streaming Rules\n");
    retranslate(); /* its label */
    return 0;
}
static int in_button(void *component) { return button_root && component && under(transform(component),button_root); }

/* ConfigTopPartsMain::OnExecutedScenePartsMain(this, source, data): a press of the button. */
static ABI int top_executed(HookRegs *r) { button_pressed=button && r->rsi==(uint64_t)(uintptr_t)button; return 0; }
/* ScenePartsMain.UIObjectGroup::GetUIObjectInfo(this, uiObject): the help bar (Back, Select) of a
 * focused button. The button has no entry of its own (the group's list is full), so it gets Logo
 * Usage's. */
static ABI int object_info(HookRegs *r) {
    if (button && r->rsi==(uint64_t)(uintptr_t)button) r->rsi=(uint64_t)(uintptr_t)logo_button;
    return 0;
}
/* ConfigTopPartsMain::sendSelectReaction(this, uiObject): the description shown next. */
static ABI int top_select(HookRegs *r) { describing=button && r->rsi==(uint64_t)(uintptr_t)button; return 0; }

/* ConfigMain::switchLayer(this, layer): the Display Settings button opens the MS Settings page in
 * display mode; leaving it saves the settings and puts the game's texts back. */
static ABI int switch_layer(HookRegs *r) {
    const int layer=(int)r->rsi;
    if (layer==LOGO_LAYER && button_pressed) { button_pressed=0; display_mode=1; r->rsi=MUSIC_STAGE_LAYER; }
    else if (layer!=MUSIC_STAGE_LAYER && display_mode) {
        display_mode=0; /* the rows keep their texts until the page opens as MS Settings */
        if (display_dirty) { display_dirty=0; theatrhythm_display_save(); }
    }
    return 0;
}

/* The child of transform `t` named `name`, or NULL. */
static unsigned char *child_named(unsigned char *t, const char *name) {
    for (int32_t i=0;t && i<child_count(t);++i) {
        unsigned char *c=child(t,i);
        char n[48]; guest_string(object_name(c),n,sizeof(n));
        if (!strcmp(n,name)) return c;
    }
    return NULL;
}
/* The label (Text_Category) of a row view, under the category node `category`. */
static unsigned char *row_label(const unsigned char *view, const char *category) {
    return view ? child_named(child_named((unsigned char *)load(view+0x18),category),"Text_Category") : NULL; /* _rectTransform */
}
static unsigned char *first_view(const unsigned char *item) {
    const unsigned char *views=item ? (const unsigned char *)load(item+0x10) : NULL;
    return views && load32(views+0x18)>0 ? (unsigned char *)load((const unsigned char *)load(views+0x10)+0x20) : NULL;
}
/* Window mode and Resolution reuse MS Settings rows whose category node (CategoryName_MsMark) holds,
 * left of the label, the marks of the stage types the setting is for (BMS_FMS, EMS). On the Display
 * Settings page they are hidden and the label moves to the plain rows' place (VSync's CategoryName);
 * the view gets both back when it shows another row. */
static void mark_row(unsigned char *view, int hide) {
    int slot=-1;
    for (int i=0;i<2;++i) if (marked[i].view==view || (hide && slot<0 && !marked[i].view)) slot=i;
    if (slot<0 || (hide && marked[slot].view==view && marked[slot].moved)) return; /* nothing to undo, or done */
    unsigned char *category=child_named((unsigned char *)load(view+0x18),"CategoryName_MsMark");
    if (!category) return;
    Vec3 (ABI *get_local)(void *, void *)=(Vec3 (ABI *)(void *, void *))CODE(C_GET_LOCAL_POSITION);        /* Transform::localPosition */
    void (ABI *set_local)(void *, Vec3, void *)=(void (ABI *)(void *, Vec3, void *))CODE(C_SET_LOCAL_POSITION);
    const int first=marked[slot].view!=view, marks=!hide || first; /* the marks show or hide now */
    for (int32_t i=0;i<child_count(category);++i) {
        unsigned char *c=child(category,i);
        char n[48]; guest_string(object_name(c),n,sizeof(n));
        if (!strcmp(n,"Text_Category")) {
            /* VSync's view may not be set up yet as the page opens: the label moves on a later call */
            unsigned char *plain=hide ? row_label(first_view(row_item[ROW_VSYNC]),"CategoryName") : NULL;
            Rect (ABI *rect)(void *, void *)=(Rect (ABI *)(void *, void *))CODE(C_RECT); /* RectTransform::rect (from the pivot) */
            Vec3 at=get_local(c,NULL);
            if (first) marked[slot].x=at.x;
            if (!hide) at.x=marked[slot].x;
            else if (plain) at.x=get_local(plain,NULL).x+rect(plain,NULL).x-rect(c,NULL).x; /* left edges level: the texts are left-aligned */
            set_local(c,at,NULL);
            marked[slot].moved=plain!=NULL;
        } else if (marks && strcmp(n,"Active")) { /* the selection highlight stays */
            void *go=((void *(ABI *)(void *, void *))CODE(C_GET_GAME_OBJECT))(c,NULL); /* Component::get_gameObject */
            if (go) ((void (ABI *)(void *, int, void *))CODE(C_SET_ACTIVE))(go,!hide,NULL); /* GameObject::SetActive */
        }
    }
    marked[slot].view=hide ? view : NULL;
}
/* ConfigListItemView::OnSetView(this, viewParameter): a view shows the row whose ViewParameter it
 * gets. */
static ABI int row_view(HookRegs *r) {
    int row=-1;
    for (int i=0;i<ROWS;++i) if (row_item[i] && r->rsi==load(row_item[i]+0x38)) row=i;
    mark_row((unsigned char *)r->rdi,display_mode && (row==ROW_WINDOW || row==ROW_RESOLUTION));
    for (int i=0;display_mode && row==ROW_VSYNC && i<2;++i) /* its label is where the others' go */
        if (marked[i].view && !marked[i].moved) mark_row(marked[i].view,1);
    return 0;
}

/* ConfigMusicStageEditPartsMain::InitializeList(this), as the page opens: when the page last listed
 * the other set of rows, list them again (OnInitializeSetting sorts with filterItem), with the
 * cursor on the first row. */
static ABI int initialize_list(HookRegs *r) {
    unsigned char *self=(unsigned char *)r->rdi;
    note_page(self);
    if (page_display==display_mode) {
        if (display_mode) for (int row=0;row<ROWS;++row) show_row(row);
        return 0;
    }
    page_display=display_mode;
    resorting=1;
    ((void (ABI *)(void *, void *))CODE(C_ON_INITIALIZE_SETTING))(self,NULL); /* OnInitializeSetting */
    resorting=0;
    put(self+0xa8,0); /* _selectedItem */
    retranslate();
    if (display_mode) for (int row=0;row<ROWS;++row) show_row(row); /* after the value texts' own terms */
    return 0;
}

/* ConfigListItemBase::InitializeSetting(this) while the page lists its rows again: only the value
 * from the game's settings (the first call already subscribed the row to language changes). */
static ABI int initialize_setting(HookRegs *r) {
    if (!resorting) return 0;
    unsigned char *item=(unsigned char *)r->rdi;
    const unsigned char *backup=(const unsigned char *)load(item+0x48); /* Func<int> _getIndexFromBackup */
    if (backup) {
        const int index=((int (ABI *)(void *, void *))load(backup+0x18))((void *)load(backup+0x40),(void *)load(backup+0x28));
        put32((unsigned char *)load(item+0x38)+0x14,index);
    }
    return 1;
}

/* ConfigMusicStageEditPartsMain::filterItem(this, listItem): display mode lists only its rows. */
static ABI int filter_item(HookRegs *r) {
    note_page((unsigned char *)r->rdi);
    unsigned char *item=(unsigned char *)r->rsi;
    if (!item || strncmp(class_name(item),"ConfigListItem",14)) return 0;
    char term[64]; guest_string((const void *)load(item+0x20),term,sizeof(term));
    int row=-1; /* the pills' in-game twins (sliders) share their help term */
    for (int i=0;i<ROWS;++i)
        if (!strcmp(term,rows[i].detail) && (i>ROW_RESOLUTION || !strcmp(class_name(item),"ConfigListItemButton"))) {
            row_item[i]=item; row=i;
        }
    page_display=display_mode; /* every sort (the game's own too, as the scene is set up again) */
    if (!display_mode) return 0;
    r->rax=row>=0;
    return 1;
}

/* ConfigMusicStageEditPartsMain::compareItem(this, a, b): display mode orders its rows. */
static ABI int compare_item(HookRegs *r) {
    const int a=row_of((unsigned char *)r->rsi), b=row_of((unsigned char *)r->rdx);
    if (!display_mode || a<0 || b<0) return 0;
    r->rax=(uint64_t)(int64_t)(a-b);
    return 1;
}

/* ConfigListItemButton::OnSubmit(this): the pills step to their next value. */
static ABI int submit(HookRegs *r) {
    unsigned char *item=(unsigned char *)r->rdi;
    const int row=display_mode ? row_of(item) : -1;
    if (row!=ROW_WINDOW && row!=ROW_RESOLUTION) return 0;
    play_se((const void *)load(item+0x58));
    if (row==ROW_WINDOW) theatrhythm_display_set(THEATRHYTHM_DISPLAY_MODE,(theatrhythm_display_get(THEATRHYTHM_DISPLAY_MODE)+1)%3);
    else {
        int w,h; const int count=theatrhythm_display_resolution(-1,&w,&h);
        if (count>0) theatrhythm_display_set(THEATRHYTHM_DISPLAY_RESOLUTION,(theatrhythm_display_get(THEATRHYTHM_DISPLAY_RESOLUTION)+1)%count);
    }
    display_dirty=1;
    show_row(row);
    return 1;
}

/* ConfigListItemSlider/Selector::OnHorizontalInput(this, direction 0 left / 1 right) -> handled:
 * VSync and the render resolution (selectors) and the frame rate limit (a slider) change the
 * runner's settings instead of the game's (no _onChangeData). Both keep the value count at +0x50;
 * `se`: the select sound's field. */
static int horizontal(HookRegs *r, size_t se) {
    unsigned char *item=(unsigned char *)r->rdi;
    const int row=display_mode ? row_of(item) : -1;
    if (row!=ROW_VSYNC && row!=ROW_FPS && row!=ROW_RENDER) return 0;
    unsigned char *parameter=(unsigned char *)load(item+0x38);
    const int old=load32(parameter+0x14), stops=load32(item+0x50);
    const int direction=(int)r->rsi;
    int index=direction==1 ? old+1 : direction==0 ? old-1 : old;
    if (index>stops-1) index=stops-1;
    if (index<0) index=0;
    r->rax=index!=old;
    if (index==old) return 1;
    put32(parameter+0x14,index);
    if (row==ROW_VSYNC) theatrhythm_display_set(THEATRHYTHM_DISPLAY_VSYNC,index==0);
    else if (row==ROW_RENDER) theatrhythm_display_set(THEATRHYTHM_DISPLAY_RENDER_4K,index==1);
    else if (index<6) theatrhythm_display_set(THEATRHYTHM_DISPLAY_FPS_LIMIT,fps_stops[index]);
    refresh_view(item);
    display_dirty=1;
    play_se((const void *)load(item+se));
    return 1;
}
static ABI int slider_horizontal(HookRegs *r) { return horizontal(r,0x68); }
static ABI int selector_horizontal(HookRegs *r) { return horizontal(r,0x58); }

/* ConfigMusicStageEditPartsMain::openCheckDefaultWindow(this): no Restore defaults in display mode. */
static ABI int check_default(HookRegs *r) { (void)r; return display_mode; }
/* HelpBarContent::RefleshDisplay(this, device, buttonNames, text colors) as HelpBar::Draw shows an
 * entry (_contentsData already set): in display mode Restore defaults gets no button icon, and Draw
 * hides an entry without one. */
static ABI int help_entry(HookRegs *r) {
    unsigned char *content=(unsigned char *)r->rdi;
    const unsigned char *data=display_mode ? (const unsigned char *)load(content+0x38) : NULL; /* _contentsData */
    char term[48];
    if (!data) return 0;
    guest_string((const void *)load(data+0x10),term,sizeof(term)); /* _term */
    if (strcmp(term,"Common/HelpBar/Default")) return 0;
    void *icon=(void *)load(content+0x20); /* _imageButtonIcon */
    if (icon) ((void (ABI *)(void *, void *, void *))CODE(C_SET_SPRITE))(icon,NULL,NULL); /* Image::set_sprite */
    return 1;
}

/* A row's value as a plain text in its behaviour's LocalizedText at `field` (_valueText.text = ...,
 * the virtual setter as the game calls it for a programmable value). */
static void value_text(unsigned char *behaviour, size_t field, int row) {
    unsigned char *text=(unsigned char *)load(behaviour+field);
    if (!text) return;
    char value[48];
    if (row==ROW_WINDOW) snprintf(value,sizeof(value),"%s",mode_names[theatrhythm_display_get(THEATRHYTHM_DISPLAY_MODE)%3]);
    else if (row==ROW_RESOLUTION) {
        int w=0,h=0;
        theatrhythm_display_resolution(theatrhythm_display_get(THEATRHYTHM_DISPLAY_RESOLUTION),&w,&h);
        snprintf(value,sizeof(value),"%d x %d",w,h);
    } else {
        const int32_t index=load32(behaviour+0x18); /* SelectedIndex */
        const int unlimited=index==5 && theatrhythm_display_get(THEATRHYTHM_DISPLAY_FPS_LIMIT)<0;
        snprintf(value,sizeof(value),"%s",unlimited ? "Unlimited" : fps_ticks[index>=0 && index<6 ? index : 0]);
    }
    void *string=guest_text((void *)load(row_item[row]+0x20),value); /* a string's class: the help term's */
    const unsigned char *klass=(const unsigned char *)load(text);
    if (string) ((void (ABI *)(void *, void *, void *))load(klass+0x5e8))(text,string,(void *)load(klass+0x5f0));
}
/* ConfigButtonBehaviour::refreshValueText(this) and SetIndex(this, index) (which inlines it): the
 * pills show the runner's value. */
static ABI int button_value(HookRegs *r) {
    unsigned char *behaviour=(unsigned char *)r->rdi;
    const int row=display_mode ? row_of_behaviour(behaviour) : -1;
    if (row!=ROW_WINDOW && row!=ROW_RESOLUTION) return 0;
    value_text(behaviour,0x20,row);
    return 1;
}
static ABI int button_set_index(HookRegs *r) {
    if (!display_mode || row_of_behaviour((void *)r->rdi)<0) return 0;
    put32((unsigned char *)r->rdi+0x18,(int32_t)r->rsi);
    return button_value(r);
}
/* ConfigSliderBehaviour::refreshValueText(this) and SetIndex(this, index) (which moves _slider and
 * inlines it): the frame rate limit's box shows FPS, not the stop's number. */
static ABI int slider_value(HookRegs *r) {
    unsigned char *behaviour=(unsigned char *)r->rdi;
    if (!display_mode || row_of_behaviour(behaviour)!=ROW_FPS) return 0;
    value_text(behaviour,0x30,ROW_FPS);
    return 1;
}
static ABI int slider_set_index(HookRegs *r) {
    unsigned char *behaviour=(unsigned char *)r->rdi;
    if (!display_mode || row_of_behaviour(behaviour)!=ROW_FPS) return 0;
    put32(behaviour+0x18,(int32_t)r->rsi);
    unsigned char *slider=(unsigned char *)load(behaviour+0x20); /* _slider.value = index */
    const unsigned char *klass=(const unsigned char *)load(slider);
    ((void (ABI *)(void *, float, void *))load(klass+0x428))(slider,(float)(int32_t)r->rsi,(void *)load(klass+0x430));
    value_text(behaviour,0x30,ROW_FPS);
    return 1;
}

/* LocalizedTextBase<T>::SetTextByTermInfo(this) (shared generic code): the text being translated. */
static ABI int set_text_by_term(HookRegs *r) { label_text=(void *)r->rdi; return 0; }
/* LocalizedTextBase<T>::onLanguageChanged(this, from, to): skips a text whose language is `to`, so
 * retranslate() translates the rows' texts itself (SetTextByTermInfo of the same class: its code
 * only reads the class from the MethodInfo). */
static int on_language_changed(HookRegs *r, int set_text) {
    if (!forcing || (row_holding((void *)r->rdi)<0 && !in_button((void *)r->rdi))) return 0;
    ((void (ABI *)(void *, void *))CODE(set_text))((void *)r->rdi,(void *)r->rcx);
    return 1;
}
static ABI int on_language_changed_a(HookRegs *r) { return on_language_changed(r,H_SET_TEXT_A); }
static ABI int on_language_changed_b(HookRegs *r) { return on_language_changed(r,H_SET_TEXT_B); }

/* I2 LocalizationManager::TryGetTranslation(term, out translation, ...): the Display Settings texts. */
static ABI int translate(HookRegs *r) {
    void *component=label_text; label_text=NULL;
    char term[80]; guest_string((const void *)r->rdi,term,sizeof(term));
    const char *text=runtime_exit_prompt_translation(term); /* F4's "Exit the game?" window */
    if (text) {}
    else if (!strcmp(term,"Scene/Config/TopButton/007") && in_button(component)) text="Display Settings";
    else if (describing && !strcmp(term,"Common/DetailText/Config0007")) /* Logo Usage's description */
        text="Change the window mode, resolution, VSync, frame rate limit and render resolution";
    else if (display_mode) {
        if (!strcmp(term,"Scene/Config/TopButton/002")) text="Display Settings";
        else if (!strcmp(term,"Scene/Config/MsSettings/004")) /* both pills */
            text=component && in_row(component,row_item[ROW_RESOLUTION]) ? "Resolution" : "Window mode";
        else if (!strcmp(term,"Scene/Config/MsSettings/010")) text="VSync";
        else if (!strcmp(term,"Scene/Config/MsSettings/017")) text="Frame rate limit";
        else if (!strcmp(term,"Scene/Config/MsSettings/011")) text="Render resolution";
        else if ((!strcmp(term,"Scene/Config/On") || !strcmp(term,"Scene/Config/Off")) && component &&
                 in_row(component,row_item[ROW_RENDER])) /* the selector's two values */
            text=term[14]=='n' ? "1080p" : "4K";
        else if (!strncmp(term,"Scene/Config/",13) && term[13]>='1' && term[13]<='5' && !term[14]) text=fps_ticks[term[13]-'0'];
        else if (!strcmp(term,"Common/HelpBar/DedicatedScene")) { /* the pills' "Open setting window" */
            void *swap=guest_text((const void *)r->rdi,"Common/HelpBar/SwitchSetting");
            if (swap) r->rdi=(uint64_t)(uintptr_t)swap;
            return 0;
        } else for (int i=0;i<ROWS;++i) if (!strcmp(term,rows[i].detail)) text=rows[i].help;
    }
    void *translation=text ? guest_text((const void *)r->rdi,text) : NULL;
    if (!translation) return 0;
    put((void *)r->rsi,(uint64_t)(uintptr_t)translation);
    r->rax=1;
    return 1;
}

/* All nine display languages: the game has Korean and Chinese (Traditional, Simplified) text too, but
 * this release allows only the Western six (RegionLanguage's World region). On Options > Language &
 * menu volume, left and right on Display language step through all nine; pressing it still opens the
 * game's picker of six. SystemLanguage values, in the picker's order and then the three it leaves out. */
static const int32_t languages[]={15,10,34,14,21,22,23,41,40};

/* ConfigLanguageMenuEditPartsMain::OnExecutedScenePartsMain(this, reaction) -> handled: left (action
 * 0) or right (1) on Display language, the page's only button row, switches the language as the picker
 * does: LanguageController.SwitchLanguage(language, FontResourceManager.Instance), which loads the
 * language's fonts (not awaited), Backup.ConfigInfo.DisplayLanguage, then
 * Backup.ReserveSave. */
static ABI int language_input(HookRegs *r) {
    unsigned char *self=(unsigned char *)r->rdi, *reaction=(unsigned char *)r->rsi;
    const int32_t action=load32(reaction+0x20);
    const unsigned char *list=action==0 || action==1 ? (const unsigned char *)load(self+0x90) : NULL;
    const unsigned char *selected=list ? (const unsigned char *)load(list+0xa8) : NULL; /* SelectedItemInfo */
    unsigned char *item=selected ? (unsigned char *)load(selected+0x20) : NULL;
    if (!item || strcmp(class_name(item),"ConfigListItemButton")) return 0;
    unsigned char *controller=((unsigned char *(ABI *)(void *))CODE(C_LANGUAGE_CONTROLLER))(NULL); /* LanguageController.Instance */
    void *fonts=((void *(ABI *)(void *))CODE(C_FONT_RESOURCES))(NULL); /* FontResourceManager.Instance */
    if (!controller || !fonts) return 0;
    const int count=sizeof(languages)/sizeof(*languages);
    int at=1; /* English */
    for (int i=0;i<count;++i) if (languages[i]==load32(controller+0x18)) at=i; /* CurrentLanguage */
    const int32_t language=languages[(at+(action==1 ? 1 : count-1))%count];
    ((void (ABI *)(void *, int32_t, void *, void *))CODE(C_SWITCH_LANGUAGE))(controller,language,fonts,NULL);
    unsigned char *config=((unsigned char *(ABI *)(void *))CODE(C_CONFIG_INFO))(NULL); /* Backup::get_ConfigInfo */
    if (config) put32(config+0x1c,language);
    ((void (ABI *)(void *))CODE(C_RESERVE_SAVE))(NULL); /* Backup::ReserveSave */
    play_se((const void *)load(item+0x58));
    ((void (ABI *)(void *, int, void *))CODE(C_END_ACTION))(reaction,0,NULL); /* UIObject::EndAction */
    r->rax=0;
    return 1;
}
/* Code that would turn the three back into English: the region checks return the language as asked,
 * and ConfigInfo::set_DisplayLanguage and the save's reader keep it as given (both check the World
 * region inline). A new save's default, from the system language, stays one of the six. */
static const struct { int site; unsigned char length, patch[4]; } language_patches[]={
    {P_REGION_CURRENT,3,{0x89,0xf8,0xc3}},          /* RegionLanguage::GetLanguageAllowedInCurrentRegion: mov eax,edi; ret */
    {P_REGION_WORLD,3,{0x89,0xf8,0xc3}},            /* ...InWorldRegion */
    {P_REGION_JAPAN,3,{0x89,0xf8,0xc3}},            /* ...InJapanRegion */
    {P_SET_DISPLAY_LANGUAGE,4,{0x89,0x77,0x1c,0xc3}}, /* ConfigInfo::set_DisplayLanguage: mov [rdi+0x1c],esi; ret */
    {P_DESERIALIZE,4,{0x89,0xc1,0xeb,0x37}},        /* ConfigInfoFormatter::Deserialize, DisplayLanguage: mov ecx,eax; jmp to its store */
};
/* The picker (WindowLanguageSettingBodyPartsMain's InitializeSetting, MoveNext) selects the button of
 * the current language, read at P_PICKER: mov r12d,[rax+0x18]; jmp the button loop. A language it
 * has no button for would leave it without a cursor, so a stub reads those three as English. */
#define PICKER_SITE_LENGTH 6

static const Hook hooks[]={
    {H_SWITCH_LAYER, switch_layer, 0},              /* ConfigMain::switchLayer */
    {H_TRANSLATE, translate, 0},                    /* LocalizationManager::TryGetTranslation */
    {H_SET_TEXT_A, set_text_by_term, 0},            /* LocalizedTextBase<T>::SetTextByTermInfo */
    {H_SET_TEXT_B, set_text_by_term, 0},            /* (its other instance) */
    {H_LANGUAGE_CHANGED_A, on_language_changed_a, 0}, /* LocalizedTextBase<T>::onLanguageChanged */
    {H_LANGUAGE_CHANGED_B, on_language_changed_b, 0}, /* (its other instance) */
    {H_BUTTON_VALUE, button_value, 0},              /* ConfigButtonBehaviour::refreshValueText */
    {H_BUTTON_SET_INDEX, button_set_index, 0},      /* ConfigButtonBehaviour::SetIndex */
    {H_SLIDER_VALUE, slider_value, 0},              /* ConfigSliderBehaviour::refreshValueText */
    {H_SLIDER_SET_INDEX, slider_set_index, 0},      /* ConfigSliderBehaviour::SetIndex */
    {H_INITIALIZE_LIST, initialize_list, 0},        /* ConfigMusicStageEditPartsMain::InitializeList */
    {H_INITIALIZE_SETTING, initialize_setting, 0},  /* ConfigListItemBase::InitializeSetting */
    {H_FILTER_ITEM, filter_item, 0},                /* ConfigMusicStageEditPartsMain::filterItem */
    {H_COMPARE_ITEM, compare_item, 0},              /* ConfigMusicStageEditPartsMain::compareItem */
    {H_SUBMIT, submit, 0},                          /* ConfigListItemButton::OnSubmit */
    {H_SLIDER_HORIZONTAL, slider_horizontal, 0},    /* ConfigListItemSlider::OnHorizontalInput */
    {H_SELECTOR_HORIZONTAL, selector_horizontal, 0}, /* ConfigListItemSelector::OnHorizontalInput */
    {H_CHECK_DEFAULT, check_default, 0},            /* ConfigMusicStageEditPartsMain::openCheckDefaultWindow */
    {H_TOP_SWITCH_IN, top_switch_in, 0},            /* ConfigTopLayerMain::SwitchIn */
    {H_TOP_EXECUTED, top_executed, 0},              /* ConfigTopPartsMain::OnExecutedScenePartsMain */
    {H_TOP_SELECT, top_select, 0},                  /* ConfigTopPartsMain::sendSelectReaction */
    {H_OBJECT_INFO, object_info, 0},                /* ScenePartsMain.UIObjectGroup::GetUIObjectInfo */
    {H_ROW_VIEW, row_view, 0},                      /* ConfigListItemView::OnSetView */
    {H_HELP_ENTRY, help_entry, 6},                  /* HelpBarContent::RefleshDisplay (3 Colors) */
    {H_LANGUAGE_INPUT, language_input, 0},          /* ConfigLanguageMenuEditPartsMain::OnExecutedScenePartsMain */
};

static unsigned char *emit(unsigned char *p, const void *bytes, size_t size) { memcpy(p,bytes,size); return p+size; }
/* mov [rsp+disp], reg (store) or mov reg, [rsp+disp] (load); reg in x86 numbering. */
static unsigned char *rsp_slot(unsigned char *p, int store, int reg, uint8_t disp) {
    const unsigned char bytes[]={(unsigned char)(0x48|(reg>=8 ? 4 : 0)),store ? 0x89 : 0x8b,(unsigned char)(0x44|(reg&7)<<3),0x24,disp};
    return emit(p,bytes,sizeof bytes);
}
/* movups [rsp+disp], xmmN (store) or movups xmmN, [rsp+disp] (load); N below 8. */
static unsigned char *xmm_slot(unsigned char *p, int store, int reg, int32_t disp) {
    const unsigned char bytes[]={0x0f,store ? 0x11 : 0x10,(unsigned char)(0x84|reg<<3),0x24};
    p=emit(p,bytes,sizeof bytes);
    return emit(p,&disp,4);
}
/* Most bytes emit_hook_stub writes for `h`. */
static size_t stub_size(const Hook *h) { return 144+16*(size_t)h->floats; } /* 16 moved bytes at most */
/* The stub for `h` at `p`: save the argument registers in a HookRegs on the stack, and after it the
 * xmm argument registers the function takes (the call stays 16-byte aligned: rsp is 8 mod 16 at the
 * function entry), call h->fn, then return its result or restore them, run the moved prologue and
 * jump back into the function. */
static unsigned char *emit_hook_stub(unsigned char *p, const Hook *h, unsigned char *function) {
    static const int args[6]={7,6,2,1,8,9}; /* rdi rsi rdx rcx r8 r9 */
    const int32_t frame=72+16*h->floats;
    p=emit(p,(const unsigned char[]){0x48,0x81,0xec},3); p=emit(p,&frame,4); /* sub rsp,frame */
    for (int i=0;i<6;++i) p=rsp_slot(p,1,args[i],(uint8_t)(8*i));
    for (int i=0;i<h->floats;++i) p=xmm_slot(p,1,i,72+16*i);
    p=emit(p,(const unsigned char[]){0x48,0x89,0xe7},3);                       /* mov rdi,rsp */
    const uint64_t fn=(uint64_t)(uintptr_t)h->fn;
    p=emit(p,(const unsigned char[]){0x48,0xb8},2); p=emit(p,&fn,8);          /* mov rax,fn */
    p=emit(p,(const unsigned char[]){0xff,0xd0,0x85,0xc0},4);                 /* call rax; test eax,eax */
    unsigned char *jnz=p; p=emit(p,(const unsigned char[]){0x75,0},2);        /* jnz ret */
    for (int i=0;i<6;++i) p=rsp_slot(p,0,args[i],(uint8_t)(8*i));
    for (int i=0;i<h->floats;++i) p=xmm_slot(p,0,i,72+16*i);
    p=emit(p,(const unsigned char[]){0x48,0x81,0xc4},3); p=emit(p,&frame,4); /* add rsp,frame */
    p=emit(p,function,code_at[h->site].moved);                                     /* the moved bytes */
    const uint64_t back=(uint64_t)(uintptr_t)(function+code_at[h->site].moved);
    p=emit(p,(const unsigned char[]){0xff,0x25,0,0,0,0},6); p=emit(p,&back,8); /* jmp [rip]: back */
    jnz[1]=(unsigned char)(p-(jnz+2));
    p=rsp_slot(p,0,0,48); p=rsp_slot(p,0,2,56);                               /* rax, rdx = results */
    p=emit(p,(const unsigned char[]){0x48,0x81,0xc4},3); p=emit(p,&frame,4); /* add rsp,frame */
    return emit(p,(const unsigned char[]){0xc3},1);                           /* ret */
}
/* The picker's stub at `p`, jumped to from P_PICKER: its load, the three as English, then back to
 * the button loop (the site's jmp). */
#define PICKER_STUB_SIZE 42
static unsigned char *emit_picker_stub(unsigned char *p) {
    static const unsigned char code[]={
        0x44,0x8b,0x60,0x18,                /* mov r12d,[rax+0x18] */
        0x41,0x83,0xfc,0x17, 0x74,0x0c,     /* cmp r12d,23 (Korean); je english */
        0x41,0x83,0xfc,0x28, 0x74,0x06,     /* cmp r12d,40 (ChineseSimplified); je english */
        0x41,0x83,0xfc,0x29, 0x75,0x06,     /* cmp r12d,41 (ChineseTraditional); jne back */
        0x41,0xbc,0x0a,0x00,0x00,0x00,      /* english: mov r12d,10 */
        0xff,0x25,0,0,0,0};                 /* back: jmp [rip] */
    const unsigned char *site=CODE(P_PICKER);
    const uint64_t loop=(uint64_t)(uintptr_t)(site+PICKER_SITE_LENGTH+(int8_t)site[5]);
    p=emit(p,code,sizeof code);
    return emit(p,&loop,8);
}

/* Display Settings stays on F10 alone: logged, and told on screen as the game starts. */
static int not_installed(const char *why) {
    if (why) printf("Display page: %s\n",why);
    printf("Display page: the Options menu stays as it is in game %s; Display Settings is on F10\n",runtime_game_version());
    char notice[160];
    snprintf(notice,sizeof(notice),"Display Settings is not in Options for game %s yet: press F10 for it",runtime_game_version());
    theatrhythm_gpu_notice(notice,20000);
    return -1;
}

int runtime_display_page_install(unsigned char *image, uint64_t module_base, unsigned char *stubs, size_t size, int monitor) {
    module=image+module_base;
    monitor_icon=monitor;
    if (runtime_game_code_find(sites,SITES,code_at,"Display page")) return not_installed(NULL);
    size_t need=0;
    for (size_t i=0;i<sizeof(hooks)/sizeof(*hooks);++i) {
        const unsigned moved=code_at[hooks[i].site].moved;
        if (moved<5 || moved>16) return not_installed("a hook's moved bytes are not 5 to 16 (runner bug)");
        need+=stub_size(&hooks[i]);
        const int64_t rel=(int64_t)(stubs+need-(module+code_at[hooks[i].site].offset+5)); /* the farthest its stub can be */
        if (rel!=(int32_t)rel || need>size) return not_installed("no room for the hook stubs");
    }
    need+=PICKER_STUB_SIZE;
    if (need>size) return not_installed("no room for the hook stubs");
    for (size_t i=0;i<sizeof(language_patches)/sizeof(*language_patches);++i)
        memcpy(CODE(language_patches[i].site),language_patches[i].patch,language_patches[i].length);
    unsigned char *p=stubs;
    for (size_t i=0;i<sizeof(hooks)/sizeof(*hooks);++i) {
        unsigned char *function=CODE(hooks[i].site);
        unsigned char *stub=p;
        p=emit_hook_stub(p,&hooks[i],function);
        const int32_t rel32=(int32_t)(stub-(function+5));
        function[0]=0xe9; memcpy(function+1,&rel32,4);
        memset(function+5,0x90,code_at[hooks[i].site].moved-5u); /* never run: the stub jumps past them */
    }
    unsigned char *site=CODE(P_PICKER);
    const int32_t rel32=(int32_t)(p-(site+5));
    p=emit_picker_stub(p);
    site[0]=0xe9; memcpy(site+1,&rel32,4); site[5]=0x90;
    printf("Display page: Options > Display Settings and all display languages (%zu hooks, game %s)\n",
           sizeof(hooks)/sizeof(*hooks),runtime_game_version());
    return 0;
}
