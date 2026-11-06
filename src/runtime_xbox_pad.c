/* The Xbox pad on the Live Info panel. runtime_button_icons.c swaps the pad atlas for the Xbox drawing
 * (scripts/controller_pad.py); this fits the pad prefab to it. The prefab (InputController: MSGamePad,
 * MSInputController) draws its base with the DualShock 4's own sprite, whose tight mesh follows the
 * DS4's outline and would cut the Xbox drawing, and puts the red highlights over the DS4's buttons.
 *
 * MSGamePad::UpdateButton and MSGamePad::GetState, which the panel and the tutorial call every frame,
 * enter through a stub that first calls update() with the pad. While the atlas shows the Xbox drawing and the base renderer shows one of
 * the DS4 base sprites (MSInputController._baseSpries), the renderer gets a FullRect sprite over
 * the same atlas area, Sprite.CreateSprite(texture, rect, pivot, 100 pixels per unit, FullRect),
 * and the highlight groups and highlights below ButtonRoot move onto the Xbox buttons. Back on the
 * DualShock 4 (the base shows another sprite), the renderer gets the DS4 sprite of the layer
 * ButtonRoot's scale says and the highlights go back where the prefab has them. Each group
 * transform is also where the tutorial's guide marks sit, so they move with it. The sites are
 * found by name and bytes (runtime_game_code.c): another game version keeps the DS4 layout. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>

enum {
    UPDATE_BUTTON, GET_STATE, GET_SPRITE, SET_SPRITE, SPRITE_GET_TEXTURE, SPRITE_GET_RECT, CREATE_SPRITE,
    CHILD_COUNT, GET_CHILD, GET_NAME, GET_POSITION, SET_POSITION, GET_SCALE, SET_SCALE, SITES
};
static const GameSite sites[SITES]={
    /* push rbp; mov rbp,rsp; push r15; push r14 (moved), then push r12 where the stub returns */
    {"Game.Scene.MusicStage.MSGamePad::UpdateButton", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 54 53 80 3d ?? ?? ?? ?? 00 41 89 d6 41 89 f7"}},
    /* push rbp; mov rbp,rsp; push r14 (moved), then push rbx */
    {"Game.Scene.MusicStage.MSGamePad::GetState", 0, 0, 0,
     {"55 48 89 e5 41 56 | 53 80 3d ?? ?? ?? ?? 00 49 89 fe 75 13 48 8d 3d ?? ?? ?? ??"}},
    {"UnityEngine.SpriteRenderer::get_sprite", 0, 0, 0, {""}},
    {"UnityEngine.SpriteRenderer::set_sprite", 0, 0, 0, {""}},
    {"UnityEngine.Sprite::get_texture", 0, 0, 0, {""}},
    {"UnityEngine.Sprite::get_rect", 0, 0, 0, {""}},
    {"UnityEngine.Sprite::CreateSprite", 0, 0, 0, {""}},
    {"UnityEngine.Transform::get_childCount", 0, 0, 0, {""}},
    {"UnityEngine.Transform::GetChild", 0, 0, 0, {""}},
    {"UnityEngine.Object::get_name", 0, 0, 0, {""}},
    {"UnityEngine.Transform::get_localPosition_Injected", 0, 0, 0, {""}},
    {"UnityEngine.Transform::set_localPosition_Injected", 0, 0, 0, {""}},
    {"UnityEngine.Transform::get_localScale_Injected", 0, 0, 0, {""}},
    {"UnityEngine.Transform::set_localScale_Injected", 0, 0, 0, {""}},
};
static GameCode at[SITES];
static unsigned char *module;
#define GUEST(site) ((void *)(module+at[site].offset))

typedef struct { float x, y; } Vec2;
typedef struct { float x, y, z; } Vec3;
typedef struct { float x, y, z, w; } Vec4;
typedef struct { float x, y, width, height; } Rect;

/* MSGamePad._currentController; MSInputController's _baseRenderers (SpriteRenderer[]), _buttonRoot,
 * _baseSpries (Sprite[], one per layer) and _buttonRootScales (Vector3[], one per layer). */
#define PAD_CONTROLLER 0x20
#define BASE_RENDERERS 0x28
#define BUTTON_ROOT 0x30
#define BASE_SPRITES 0x38
#define BUTTON_ROOT_SCALES 0x58

static uint64_t load(const void *p) { uint64_t v; memcpy(&v,p,8); return v; }
static int32_t load32(const void *p) { int32_t v; memcpy(&v,p,4); return v; }
/* A guest array's length and element `i` (Il2CppArray: max_length at +0x18, data at +0x20). */
static uint64_t length(const unsigned char *array) { return array ? load(array+0x18) : 0; }
static void *element(const unsigned char *array, uint64_t i) { return (void *)load(array+0x20+8*i); }
/* Two Unity objects with the same native object (m_CachedPtr). */
static int same(const unsigned char *a, const unsigned char *b) { return a && b && load(a+0x10) && load(a+0x10)==load(b+0x10); }
static const char *class_name(const void *object) { return (const char *)load((const char *)load(object)+0x10); }

/* Whether the guest System.String `s` (length at +0x10, UTF-16 from +0x14) is ASCII `text`. */
static int named(const void *s, const char *text) {
    const size_t n=strlen(text);
    if (!s || load32((const char *)s+0x10)!=(int32_t)n) return 0;
    const uint16_t *chars=(const uint16_t *)((const char *)s+0x14);
    for (size_t i=0;i<n;++i) if (chars[i]!=(unsigned char)text[i]) return 0;
    return 1;
}
/* The child of transform `t` named `name`, or NULL. */
static unsigned char *child_named(void *t, const char *name) {
    const int32_t count=((int32_t (ABI *)(void *, void *))GUEST(CHILD_COUNT))(t,NULL);
    for (int32_t i=0;i<count;++i) {
        unsigned char *c=((unsigned char *(ABI *)(void *, int32_t, void *))GUEST(GET_CHILD))(t,i,NULL);
        if (c && named(((void *(ABI *)(void *, void *))GUEST(GET_NAME))(c,NULL),name)) return c;
    }
    return NULL;
}

/* Where a transform goes, ButtonRoot-local for a group and group-local for a highlight: x, y and the
 * scale's x, y for the DS4 (as in the prefab) and the Xbox pad (icon-commission/make_controller.py,
 * xbox_highlights.json, with each group moved to the middle of its highlights). z stays. */
typedef struct { const char *name; float ds4[4], xbox[4]; } Place;
typedef struct { const char *name; float ds4[2], xbox[2]; Place parts[4]; } Group;
static const Group groups[]={
    {"Cross",{-1.504f,-0.03f},{-0.6051f,-0.5159f},{
        {"Up",{0,0.21f,1,1},{-0.0057f,0.1980f,0.8271f,0.5752f}},
        {"Down",{0,-0.21f,1,1},{0.0034f,-0.1979f,0.8271f,0.6192f}},
        {"Left",{-0.226f,0,1,1},{-0.2202f,0.0028f,0.6876f,0.8383f}},
        {"Right",{0.234f,0,-1,1},{0.2202f,0.0028f,-0.7006f,0.8383f}}}},
    {"Button",{1.5f,-0.03f},{1.1930f,0.1920f},{
        {"Up",{0,0.31f,1,1},{0.0136f,0.2936f,0.9901f,1.1267f}},
        {"Down",{0,-0.3f,1,1},{-0.0060f,-0.2936f,0.9901f,1.1267f}},
        {"Left",{-0.35f,0,1,1},{-0.3125f,-0.0167f,0.9901f,1.1267f}},
        {"Right",{0.35f,0,1,1},{0.3125f,0.0161f,0.9901f,1.1267f}}}},
    {"LeftButton",{-1.49f,1.06f},{-1.2292f,1.1522f},{
        {"L1",{0,-0.14f,1,1},{0.0804f,-0.0956f,1.3326f,0.8817f}},
        {"L2",{0.023f,0.12f,1,1},{-0.0803f,0.0956f,0.7701f,0.8767f}}}},
    {"RightButton",{1.49f,1.06f},{1.2292f,1.1522f},{
        {"R1",{0,-0.14f,-1,1},{-0.0804f,-0.0956f,-1.3326f,0.8817f}},
        {"R2",{-0.023f,0.12f,-1,1},{0.0803f,0.0956f,-0.7701f,0.8767f}}}},
    {"LeftStick",{-0.77f,-0.7f},{-1.2091f,0.1195f},{{"Button",{0,0,1,1},{0,0,0.9729f,1.0176f}}}},
    {"RightStick",{0.77f,-0.7f},{0.6050f,-0.5495f},{{"Button",{0,0,1,1},{0,0,0.9729f,1.0176f}}}},
};
/* The Xbox base sprites: the drawn part of each DS4 base sprite (its texture rect), whose whole
 * rect is 512x280 (Base_01) or 256x128 (Base_02) with the pivot in the middle. */
static const struct { float whole_width; Rect rect; Vec2 pivot; } bases[]={
    {512,{0,0,504,279},{0.501984f,0.501792f}},
    {256,{0,287,232,124},{0.495690f,0.516129f}},
};

static void move(void *t, const float *xy, const float *scale) {
    Vec3 v;
    ((void (ABI *)(void *, Vec3 *, void *))GUEST(GET_POSITION))(t,&v,NULL);
    v.x=xy[0]; v.y=xy[1];
    ((void (ABI *)(void *, Vec3 *, void *))GUEST(SET_POSITION))(t,&v,NULL);
    if (!scale) return;
    ((void (ABI *)(void *, Vec3 *, void *))GUEST(GET_SCALE))(t,&v,NULL);
    v.x=scale[0]; v.y=scale[1];
    ((void (ABI *)(void *, Vec3 *, void *))GUEST(SET_SCALE))(t,&v,NULL);
}
/* The highlights below ButtonRoot `root` where the DS4 (xbox 0) or the Xbox pad has its buttons. */
static void place(void *root, int xbox) {
    for (size_t g=0;g<sizeof(groups)/sizeof(*groups);++g) {
        unsigned char *group=child_named(root,groups[g].name);
        if (!group) continue;
        move(group,xbox ? groups[g].xbox : groups[g].ds4,NULL);
        for (const Place *p=groups[g].parts;p<groups[g].parts+4 && p->name;++p) {
            unsigned char *part=child_named(group,p->name);
            if (part) move(part,xbox ? p->xbox : p->ds4,(xbox ? p->xbox : p->ds4)+2);
        }
    }
}

/* A FullRect sprite over the atlas area of the DS4 base sprite `ds4`, or NULL. */
static void *xbox_base(void *ds4) {
    const Rect whole=((Rect (ABI *)(void *, void *))GUEST(SPRITE_GET_RECT))(ds4,NULL);
    void *texture=((void *(ABI *)(void *, void *))GUEST(SPRITE_GET_TEXTURE))(ds4,NULL);
    for (size_t i=0;texture && i<sizeof(bases)/sizeof(*bases);++i)
        if (whole.width==bases[i].whole_width) {
            const Vec4 border={0,0,0,0};
            return ((void *(ABI *)(void *, Rect, Vec2, float, uint32_t, int32_t, Vec4, int, void *))GUEST(CREATE_SPRITE))
                (texture,bases[i].rect,bases[i].pivot,100.0f,0,0,border,0,NULL);
        }
    return NULL;
}

static void set_base(const unsigned char *renderers, void *sprite) {
    for (uint64_t i=0;i<length(renderers);++i)
        if (element(renderers,i)) ((void (ABI *)(void *, void *, void *))GUEST(SET_SPRITE))(element(renderers,i),sprite,NULL);
}

/* MSGamePad::UpdateButton(this, ...) or GetState(this), at the entry: fit the pad to the atlas's drawing. */
static ABI void update(unsigned char *pad) {
    unsigned char *controller=pad ? (unsigned char *)load(pad+PAD_CONTROLLER) : NULL;
    if (!controller || strcmp(class_name(controller),"MSInputController")) return;
    const unsigned char *renderers=(const unsigned char *)load(controller+BASE_RENDERERS);
    const unsigned char *sprites=(const unsigned char *)load(controller+BASE_SPRITES);
    void *root=(void *)load(controller+BUTTON_ROOT);
    if (!length(renderers) || !element(renderers,0) || !length(sprites) || !root) return;
    void *shown=((void *(ABI *)(void *, void *))GUEST(GET_SPRITE))(element(renderers,0),NULL);
    if (!shown) return;
    uint64_t layer=length(sprites);
    for (uint64_t i=0;i<length(sprites);++i) if (same(shown,element(sprites,i))) layer=i;
    const int ds4_shown=layer<length(sprites);
    if (runtime_button_icons_pad()==BUTTON_ICONS_XBOX) {
        if (!ds4_shown) return;
        void *base=xbox_base(element(sprites,layer));
        if (!base) return;
        set_base(renderers,base);
        place(root,1);
        return;
    }
    if (ds4_shown) return;
    /* The layer whose ButtonRoot scale ButtonRoot has (Vector3[] of 12-byte elements). */
    Vec3 scale;
    ((void (ABI *)(void *, Vec3 *, void *))GUEST(GET_SCALE))(root,&scale,NULL);
    const unsigned char *scales=(const unsigned char *)load(controller+BUTTON_ROOT_SCALES);
    layer=length(sprites)-1;
    for (uint64_t i=0;i<length(scales) && i<length(sprites);++i) {
        float x; memcpy(&x,scales+0x20+12*i,4);
        if (x==scale.x) layer=i;
    }
    set_base(renderers,element(sprites,layer));
    place(root,0);
}

static unsigned char *emit(unsigned char *p, const void *bytes, size_t size) { memcpy(p,bytes,size); return p+size; }

int runtime_xbox_pad_install(unsigned char *image, uint64_t module_base, unsigned char *stubs, size_t size) {
    module=image+module_base;
    if (runtime_game_code_find(sites,SITES,at,"Xbox pad") || size<128) {
        printf("Xbox pad: the Live Info pad stays a DualShock 4\n");
        return -1;
    }
    static const int hooked[]={UPDATE_BUTTON,GET_STATE};
    unsigned char *p=stubs;
    for (size_t i=0;i<sizeof(hooked)/sizeof(*hooked);++i) {
        unsigned char *site=module+at[hooked[i]].offset, *stub=p;
        const unsigned moved=at[hooked[i]].moved;
        const uint64_t fn=(uint64_t)(uintptr_t)update, back=(uint64_t)(uintptr_t)(site+moved);
        p=emit(p,(const unsigned char[]){0x57,0x56,0x52,0x51,0x41,0x50,0x41,0x51},8); /* push rdi rsi rdx rcx r8 r9 */
        p=emit(p,(const unsigned char[]){0x48,0x83,0xec,0x08},4);                     /* sub rsp,8 (16-aligned) */
        p=emit(p,(const unsigned char[]){0x48,0xb8},2); p=emit(p,&fn,8);              /* mov rax,update */
        p=emit(p,(const unsigned char[]){0xff,0xd0},2);                               /* call rax (rdi: the pad) */
        p=emit(p,(const unsigned char[]){0x48,0x83,0xc4,0x08},4);                     /* add rsp,8 */
        p=emit(p,(const unsigned char[]){0x41,0x59,0x41,0x58,0x59,0x5a,0x5e,0x5f},8); /* pop r9 r8 rcx rdx rsi rdi */
        p=emit(p,site,moved);                                                         /* the moved instructions */
        p=emit(p,(const unsigned char[]){0xff,0x25,0,0,0,0},6); p=emit(p,&back,8);    /* jmp [rip]: into the method */
        if ((size_t)(p-stubs)>size) return -1;
        const int32_t jump=(int32_t)(stub-(site+5));
        site[0]=0xe9; memcpy(site+1,&jump,4);
        memset(site+5,0x90,moved-5); /* never run: the stub jumps past them */
    }
    printf("Xbox pad: the Live Info pad follows the button icons\n");
    return 0;
}
