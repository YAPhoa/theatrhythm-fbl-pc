/* libScePad on SDL3 gamepads, with a keyboard (and mouse buttons) as a second source. SDL events
 * are pumped by the window thread (gpu/shim/window.cpp); here state is only sampled, on the pad's
 * own host thread (host_thread).
 *
 * Gamepads map by position: the bottom face button is Cross, right Circle, left Square, top
 * Triangle (an Xbox pad's A B X Y). View/Back clicks the touchpad; a DualSense's own touchpad works.
 *
 * Default keyboard layout, by key position (key_map changes it, see apply_button_map). The game
 * reads taps from any button and slides from either stick, so each hand gets a stick and taps:
 *   WASD left stick, IJKL right stick, arrow keys d-pad (menus),
 *   Space/Enter Cross, Escape/Backspace/; Circle, F Square, H Triangle,
 *   Q L1, O R1, E L2, U R2 (Page Up/Page Down are L1/R1 too), X L3, M R3,
 *   Tab/P Options, T touchpad click.
 *
 * Two players (settings.ini players=2, or auto with two gamepads connected): the second PS4 user (runtime_services.c) has pad slot 1 with
 * its own handle, sample, rumble and gamepad; the keyboard, mouse and the scripted inputs below stay
 * with player 1 (slot 0). */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/theatrhythm_gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <SDL3/SDL.h>
#include <sys/stat.h>

#define ERR_INVALID_ARG ((int32_t)0x80920001)
#define ERR_INVALID_HANDLE ((int32_t)0x80920003)
#define ERR_ALREADY_OPENED ((int32_t)0x80920004)
#define ERR_NOT_INITIALIZED ((int32_t)0x80920005)
#define PAD_HANDLE 1 /* slot 0; slot n has handle n+1, for user n+1 */
#define PAD_SLOTS 2

enum {
    BTN_L3=0x2, BTN_R3=0x4, BTN_OPTIONS=0x8, BTN_UP=0x10, BTN_RIGHT=0x20, BTN_DOWN=0x40, BTN_LEFT=0x80,
    BTN_L2=0x100, BTN_R2=0x200, BTN_L1=0x400, BTN_R1=0x800, BTN_TRIANGLE=0x1000, BTN_CIRCLE=0x2000,
    BTN_CROSS=0x4000, BTN_SQUARE=0x8000, BTN_TOUCHPAD=0x100000,
    /* Host-only sources, never shown to the game: a touchpad click from a button or key without a
     * touch surface (play_click turns it into a touch and a click), and the keyboard's sticks. */
    CLICK=0x80000000,
    LSTICK_UP=0x00010000, LSTICK_DOWN=0x00020000, LSTICK_LEFT=0x00040000, LSTICK_RIGHT=0x00080000,
    RSTICK_UP=0x00200000, RSTICK_DOWN=0x00400000, RSTICK_LEFT=0x00800000, RSTICK_RIGHT=0x01000000,
    STICK_DIRECTIONS=LSTICK_UP|LSTICK_DOWN|LSTICK_LEFT|LSTICK_RIGHT|RSTICK_UP|RSTICK_DOWN|RSTICK_LEFT|RSTICK_RIGHT,
};
/* Keyboard map sources past the scancodes are mouse buttons: MOUSE_SOURCE + SDL_BUTTON_LEFT... */
enum { MOUSE_SOURCE=SDL_SCANCODE_COUNT };
typedef struct { uint16_t x, y; uint8_t id, reserve[3]; } PadTouch;
typedef struct {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y;
    uint8_t l2, r2, analog_padding[2];
    float orientation[4], acceleration[3], angular_velocity[3];
    uint8_t touch_count, touch_reserve[3];
    uint32_t touch_held_time;
    PadTouch touches[2];
    uint8_t connected, pad0[3];
    uint64_t timestamp;
    uint8_t extension[16];
    uint8_t connected_count, reserve[2], unique_length, unique[12];
} PadData;
typedef struct {
    float pixel_density; uint16_t resolution_x, resolution_y;
    uint8_t dead_zone_left, dead_zone_right, connection_type, connected_count;
    uint8_t connected, pad[3];
    int32_t device_class;
    uint8_t reserve[8];
} ControllerInfo;
_Static_assert(sizeof(PadData)==120,"OrbisPadData layout");
_Static_assert(sizeof(PadTouch)==8,"OrbisPadTouch layout");
_Static_assert(__builtin_offsetof(PadData,touches)==60,"OrbisPadData touch offset");
_Static_assert(__builtin_offsetof(PadData,timestamp)==80,"OrbisPadData timestamp offset");
_Static_assert(sizeof(ControllerInfo)==28,"OrbisPadControllerInformation layout");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized, opened[PAD_SLOTS], sdl_ready;
/* Local players this run. players=auto is decided once, at the first call (the UserService start,
 * before the game reads a pad): two when two gamepads are connected then, one otherwise. SDL lists
 * a pad a moment after its subsystem starts, so a single pad is looked for up to 300 ms. */
int runtime_players(void) {
    static int decided;
    const int want=theatrhythm_gpu_players();
    if (want>0) return want>=2 ? 2 : 1;
    const int known=__atomic_load_n(&decided,__ATOMIC_ACQUIRE);
    if (known) return known;
    int count=0;
    if (SDL_WasInit(SDL_INIT_GAMEPAD) || SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        for (int waited=0; waited<30 && count<2; ++waited) {
            int found=0;
            SDL_free(SDL_GetGamepads(&found));
            count=found;
            if (count<2) SDL_Delay(10);
        }
    }
    const int players=count>=2 ? 2 : 1;
    __atomic_store_n(&decided,players,__ATOMIC_RELEASE);
    printf("Runtime: players=auto: %d gamepad(s) connected, playing with %d\n",count,players);
    return players;
}
static int slots(void) { return runtime_players(); }
/* Slot of an open handle, or -1. */
static int slot_of(int32_t handle) { return handle>=1 && handle<=PAD_SLOTS && opened[handle-1] ? (int)handle-1 : -1; }
static size_t reads, vibrations;
/* The host side runs on its own thread (host_thread): opening, closing and even reading an SDL
 * gamepad can block for hundreds of ms while a Bluetooth pad connects or drops (SDL holds its
 * joystick lock meanwhile), and on the game's thread that froze the frame. The thread samples every
 * millisecond and a read copies the latest sample, so the game's threads make no gamepad calls. */
static pthread_mutex_t host_lock=PTHREAD_MUTEX_INITIALIZER;
static PadData host_sample[PAD_SLOTS];   /* under host_lock */
static char gamepad_name[PAD_SLOTS][64]={"none","none"};  /* under host_lock */
static struct { uint8_t large, small; uint32_t serial; } rumble[PAD_SLOTS]; /* under host_lock */
static int reconnect_wanted[PAD_SLOTS];  /* atomic: the pad file's reconnect token */

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u; }
/* Pad calls run on the game's own threads, so a slow one is a late frame: calls over 2 ms are
 * counted per kind (read, vibration, info, and the host thread's sample) and the first 20 printed.
 * One thread notes each kind; the shared count and start time are atomic. */
enum { SLOW_READ, SLOW_VIBRATION, SLOW_INFO, SLOW_HOST, SLOW_KINDS };
static size_t slow_calls[SLOW_KINDS], slow_shown;
static uint64_t slow_max_us[SLOW_KINDS], slow_epoch;
static void note_call(int kind, uint64_t start) {
    static const char *const names[SLOW_KINDS]={"read","vibration","info","host sample"};
    const uint64_t us=now_us()-start;
    uint64_t unset=0;
    __atomic_compare_exchange_n(&slow_epoch,&unset,start,false,__ATOMIC_RELAXED,__ATOMIC_RELAXED);
    if (us<2000) return;
    ++slow_calls[kind];
    if (us>slow_max_us[kind]) slow_max_us[kind]=us;
    if (__atomic_fetch_add(&slow_shown,1,__ATOMIC_RELAXED)<20)
        printf("Runtime: slow pad %s %.1f ms at %.3f s\n",names[kind],us/1e3,(start-__atomic_load_n(&slow_epoch,__ATOMIC_RELAXED))/1e6);
}
static uint8_t axis(int16_t v) { int x=(v+32768)>>8; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint8_t trigger(int16_t v) { int x=v>>7; return (uint8_t)(x<0 ? 0 : x>255 ? 255 : x); }
static uint16_t touch_axis(float v, int max) {
    return (uint16_t)(v<=0.0f ? 0 : v>=1.0f ? max : (int)(v*max+0.5f));
}

/* A click without a touch surface (Back/View, the T key, the pad file) is played like a finger in
 * the middle of the touchpad: the touch is reported CLICK_LEAD_US before the button, the button
 * stays at least CLICK_HOLD_US, and the touch lingers CLICK_TAIL_US after the release, as a real
 * finger would. */
enum { CLICK_LEAD_US=50000, CLICK_HOLD_US=50000, CLICK_TAIL_US=100000 };
typedef struct { bool active; uint64_t pressed, released; } Click;
static Click click[PAD_SLOTS];
static void play_click(PadData *d, int slot) {
    Click *const c=&click[slot];
    const uint64_t now=d->timestamp;
    bool source=(d->buttons & CLICK)!=0;
    d->buttons&=~CLICK;
    if ((d->buttons & BTN_TOUCHPAD) && !d->touch_count) { /* a replay, or a pad without fingers */
        d->buttons&=~BTN_TOUCHPAD;
        source=true;
    }
    if (source && (!c->active || c->released)) { c->active=true; c->pressed=now; c->released=0; }
    else if (!source && c->active && !c->released) c->released=now;
    if (!c->active) return;
    const uint64_t down=c->pressed+CLICK_LEAD_US;
    uint64_t up=down+CLICK_HOLD_US;
    if (!c->released) up=UINT64_MAX;
    else if (c->released>up) up=c->released;
    if (now>=up && now-up>=CLICK_TAIL_US) { c->active=false; return; }
    if (d->touch_count<2) {
        d->touches[d->touch_count]=(PadTouch){.x=960,.y=471,.id=d->touch_count};
        ++d->touch_count;
    }
    if (now>=down && now<up) d->buttons|=BTN_TOUCHPAD;
}

/* Host thread: one slot's open gamepad. */
typedef struct { SDL_Gamepad *g; SDL_JoystickID id; uint64_t next_scan; uint8_t connected_count; } HostPad;
#define FAILOVER_US 3000000 /* player 1's pad gone this long: player 2's pad takes its place */
/* Closes the slot's gamepad when it is gone or the pad file asked for a reconnect, and looks for one
 * every 100 ms while there is none: the first not held by another slot, so with two players the
 * first gamepad is player 1's and the next player 2's. */
static void host_gamepad(HostPad *pads, int slot, uint64_t now) {
    HostPad *p=&pads[slot];
    if (p->g && (__atomic_exchange_n(&reconnect_wanted[slot],0,__ATOMIC_ACQ_REL) || !SDL_GamepadConnected(p->g))) {
        SDL_CloseGamepad(p->g); p->g=NULL; p->id=0;
        pthread_mutex_lock(&host_lock); strcpy(gamepad_name[slot],"none"); pthread_mutex_unlock(&host_lock);
        printf("Runtime: gamepad %d closed\n",slot+1);
    }
    if (p->g || now<p->next_scan) return;
    p->next_scan=now+100000;
    __atomic_store_n(&reconnect_wanted[slot],0,__ATOMIC_RELEASE);
    int count=0;
    SDL_JoystickID *ids=SDL_GetGamepads(&count);
    for (int i=0;ids && i<count && !p->g;++i) {
        bool taken=false;
        for (int other=0;other<PAD_SLOTS;++other) if (other!=slot && pads[other].g && pads[other].id==ids[i]) taken=true;
        if (taken || !(p->g=SDL_OpenGamepad(ids[i]))) continue;
        p->id=ids[i];
        ++p->connected_count;
        const char *name=SDL_GetGamepadName(p->g);
        pthread_mutex_lock(&host_lock);
        snprintf(gamepad_name[slot],sizeof(gamepad_name[slot]),"%s",name ? name : "gamepad");
        pthread_mutex_unlock(&host_lock);
        printf("Runtime: gamepad %d connected: %s\n",slot+1,name ? name : "gamepad");
    }
    SDL_free(ids);
}
/* Button sources: a host button or key and the PS4 button it presses. The defaults below can be
 * changed per PS4 button with pad_map (gamepad) and key_map (keyboard and mouse) in settings.ini,
 * or THEATRHYTHM_PAD_MAP / THEATRHYTHM_KEY_MAP, e.g. key_map=cross=G,cross=Mouse Left,square=Left Shift
 * or pad_map=cross=b,circle=a: comma-separated <ps4 button>=<SDL key, mouse or gamepad button name>.
 * A mapped PS4 button loses its default sources, and naming it again adds a source; <ps4 button>=None
 * leaves it without any. touchpad is the touchpad click; the keyboard's stick directions (lstick_up
 * ... rstick_right) are mapped too; gamepad axes and the real touchpad are unchanged. */
static const struct { const char *name; uint32_t ps; } ps_names[]={
    {"cross",BTN_CROSS}, {"circle",BTN_CIRCLE}, {"square",BTN_SQUARE}, {"triangle",BTN_TRIANGLE},
    {"l1",BTN_L1}, {"r1",BTN_R1}, {"l2",BTN_L2}, {"r2",BTN_R2}, {"l3",BTN_L3}, {"r3",BTN_R3},
    {"options",BTN_OPTIONS}, {"touchpad",CLICK},
    {"up",BTN_UP}, {"down",BTN_DOWN}, {"left",BTN_LEFT}, {"right",BTN_RIGHT},
    {"lstick_up",LSTICK_UP}, {"lstick_down",LSTICK_DOWN}, {"lstick_left",LSTICK_LEFT}, {"lstick_right",LSTICK_RIGHT},
    {"rstick_up",RSTICK_UP}, {"rstick_down",RSTICK_DOWN}, {"rstick_left",RSTICK_LEFT}, {"rstick_right",RSTICK_RIGHT},
};
static const struct { const char *name; int button; } mouse_names[]={
    {"Mouse Left",SDL_BUTTON_LEFT}, {"Mouse Middle",SDL_BUTTON_MIDDLE}, {"Mouse Right",SDL_BUTTON_RIGHT},
    {"Mouse X1",SDL_BUTTON_X1}, {"Mouse X2",SDL_BUTTON_X2},
};
typedef struct { int source; uint32_t ps; } ButtonSource;
typedef struct { ButtonSource entries[64]; size_t count; } ButtonMap;
static ButtonMap pad_map={{
    {SDL_GAMEPAD_BUTTON_SOUTH,BTN_CROSS}, {SDL_GAMEPAD_BUTTON_EAST,BTN_CIRCLE},
    {SDL_GAMEPAD_BUTTON_WEST,BTN_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH,BTN_TRIANGLE},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,BTN_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,BTN_R1},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK,BTN_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK,BTN_R3},
    {SDL_GAMEPAD_BUTTON_START,BTN_OPTIONS}, {SDL_GAMEPAD_BUTTON_BACK,CLICK},
    {SDL_GAMEPAD_BUTTON_TOUCHPAD,BTN_TOUCHPAD},
    {SDL_GAMEPAD_BUTTON_DPAD_UP,BTN_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN,BTN_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT,BTN_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,BTN_RIGHT},
},15};
static ButtonMap key_map={{
    {SDL_SCANCODE_W,LSTICK_UP}, {SDL_SCANCODE_S,LSTICK_DOWN}, {SDL_SCANCODE_A,LSTICK_LEFT}, {SDL_SCANCODE_D,LSTICK_RIGHT},
    {SDL_SCANCODE_I,RSTICK_UP}, {SDL_SCANCODE_K,RSTICK_DOWN}, {SDL_SCANCODE_J,RSTICK_LEFT}, {SDL_SCANCODE_L,RSTICK_RIGHT},
    {SDL_SCANCODE_UP,BTN_UP}, {SDL_SCANCODE_DOWN,BTN_DOWN}, {SDL_SCANCODE_LEFT,BTN_LEFT}, {SDL_SCANCODE_RIGHT,BTN_RIGHT},
    {SDL_SCANCODE_SPACE,BTN_CROSS}, {SDL_SCANCODE_RETURN,BTN_CROSS}, {SDL_SCANCODE_KP_ENTER,BTN_CROSS},
    {SDL_SCANCODE_ESCAPE,BTN_CIRCLE}, {SDL_SCANCODE_BACKSPACE,BTN_CIRCLE}, {SDL_SCANCODE_SEMICOLON,BTN_CIRCLE},
    {SDL_SCANCODE_F,BTN_SQUARE}, {SDL_SCANCODE_H,BTN_TRIANGLE},
    {SDL_SCANCODE_Q,BTN_L1}, {SDL_SCANCODE_PAGEUP,BTN_L1}, {SDL_SCANCODE_O,BTN_R1}, {SDL_SCANCODE_PAGEDOWN,BTN_R1},
    {SDL_SCANCODE_E,BTN_L2}, {SDL_SCANCODE_U,BTN_R2}, {SDL_SCANCODE_X,BTN_L3}, {SDL_SCANCODE_M,BTN_R3},
    {SDL_SCANCODE_TAB,BTN_OPTIONS}, {SDL_SCANCODE_P,BTN_OPTIONS}, {SDL_SCANCODE_T,CLICK},
},31};

static char *trim(char *text) {
    while (*text==' ' || *text=='\t') ++text;
    char *end=text+strlen(text);
    while (end>text && (end[-1]==' ' || end[-1]=='\t' || end[-1]=='\r' || end[-1]=='\n')) *--end=0;
    return text;
}

/* Source of a key_map name: a mouse button, or a scancode by SDL's key names, plus Comma and Equals
 * for the two keys whose own names (',' and '=') are separators of the map. */
static int key_source(const char *name) {
    for (size_t i=0;i<sizeof(mouse_names)/sizeof(*mouse_names);++i)
        if (!SDL_strcasecmp(name,mouse_names[i].name)) return MOUSE_SOURCE+mouse_names[i].button;
    if (!SDL_strcasecmp(name,"Comma")) return SDL_SCANCODE_COMMA;
    if (!SDL_strcasecmp(name,"Equals")) return SDL_SCANCODE_EQUALS;
    return (int)SDL_GetScancodeFromName(name);
}

static void drop_sources(ButtonMap *map, uint32_t ps) {
    size_t kept=0;
    for (size_t i=0;i<map->count;++i) if (map->entries[i].ps!=ps) map->entries[kept++]=map->entries[i];
    map->count=kept;
}

/* Applies a pad_map/key_map value; returns how many entries were used. Bad entries are logged and
 * skipped, so their PS4 button keeps its default unless another entry names it. */
static int apply_button_map(ButtonMap *map, const char *spec, bool keyboard) {
    if (!spec || !*spec) return 0;
    char copy[1024];
    snprintf(copy,sizeof(copy),"%s",spec);
    int used=0;
    uint32_t named=0; /* PS4 buttons this spec already took the defaults from */
    char *entry=copy;
    while (entry) {
        char *next=strchr(entry,',');
        if (next) *next++=0;
        char *eq=strchr(entry,'=');
        if (!eq) {
            if (*trim(entry)) printf("Runtime: pad map: ignored '%s' (expected button=name)\n",trim(entry));
            entry=next;
            continue;
        }
        *eq=0;
        const char *button=trim(entry), *name=trim(eq+1);
        entry=next;
        uint32_t ps=0;
        for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) if (!SDL_strcasecmp(button,ps_names[i].name)) ps=ps_names[i].ps;
        if (!keyboard && (ps & STICK_DIRECTIONS)) ps=0;
        if (ps && !SDL_strcasecmp(name,"None")) { /* the button gets no source at all */
            drop_sources(map,ps);
            named|=ps;
            ++used;
            continue;
        }
        const int source=!*name ? -1 : keyboard ? key_source(name) : (int)SDL_GetGamepadButtonFromString(name);
        if (!ps || source<=(keyboard ? (int)SDL_SCANCODE_UNKNOWN : (int)SDL_GAMEPAD_BUTTON_INVALID)) {
            printf("Runtime: pad map: ignored '%s=%s'\n",button,name);
            continue;
        }
        if (!(named & ps)) { drop_sources(map,ps); named|=ps; }
        if (map->count<sizeof(map->entries)/sizeof(*map->entries)) map->entries[map->count++]=(ButtonSource){source,ps};
        ++used;
    }
    return used;
}

static void load_button_maps(void) {
    static bool loaded;
    if (loaded) return;
    loaded=true;
    const int pad=apply_button_map(&pad_map,theatrhythm_pad_map(0),false);
    const int keys=apply_button_map(&key_map,theatrhythm_pad_map(1),true);
    if (pad || keys) printf("Runtime: pad map: %d gamepad and %d keyboard entries applied\n",pad,keys);
}

static void key_axis(uint8_t *axis, bool negative, bool positive) {
    if (negative || positive) *axis=(uint8_t)(128-(negative ? 128 : 0)+(positive ? 127 : 0));
}

/* keys: SDL keyboard state (NULL without a window); mouse: SDL_MouseButtonFlags. Returns the mapped
 * sources held (0: the keyboard and mouse are idle). */
static uint32_t sample_keyboard(PadData *d, const bool *keys, uint32_t mouse) {
    uint32_t pressed=0;
    for (size_t i=0;i<key_map.count;++i) {
        const int source=key_map.entries[i].source;
        const bool down=source>=MOUSE_SOURCE ? (mouse & SDL_BUTTON_MASK(source-MOUSE_SOURCE))!=0 :
                        keys && source>0 && keys[source];
        if (down) pressed|=key_map.entries[i].ps;
    }
    d->buttons|=pressed & ~STICK_DIRECTIONS;
    if (d->buttons & BTN_L2) d->l2=255;
    if (d->buttons & BTN_R2) d->r2=255;
    key_axis(&d->left_x,pressed & LSTICK_LEFT,pressed & LSTICK_RIGHT);
    key_axis(&d->left_y,pressed & LSTICK_UP,pressed & LSTICK_DOWN);
    key_axis(&d->right_x,pressed & RSTICK_LEFT,pressed & RSTICK_RIGHT);
    key_axis(&d->right_y,pressed & RSTICK_UP,pressed & RSTICK_DOWN);
    return pressed;
}

/* Prompt icons for a pad: PlayStation ones when its bottom face button is Cross, else Xbox. */
static int pad_icons(SDL_Gamepad *g) {
    return SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_SOUTH)==SDL_GAMEPAD_BUTTON_LABEL_CROSS ?
           BUTTON_ICONS_PS4 : BUTTON_ICONS_XBOX;
}
/* A stick past half way: a resting stick's drift never gets there. */
static bool stick_pushed(const PadData *d) {
    const uint8_t axes[4]={d->left_x,d->left_y,d->right_x,d->right_y};
    for (int i=0;i<4;++i) if (axes[i]<64 || axes[i]>192) return true;
    return false;
}

/* Host thread: the gamepad (and for player 1 the keyboard and mouse) as one PadData (no timestamp). */
static void host_read(PadData *d, SDL_Gamepad *g, uint8_t connected_count, int slot) {
    memset(d,0,sizeof(*d));
    d->left_x=d->left_y=d->right_x=d->right_y=128;
    d->orientation[3]=1.0f;
    d->connected=1; d->connected_count=connected_count ? connected_count : 1;
    if (theatrhythm_gpu_overlay_captures_input()) return; /* a dialog is open: neutral input */
    const bool *k=slot==0 && SDL_WasInit(SDL_INIT_VIDEO) ? SDL_GetKeyboardState(NULL) : NULL;
    load_button_maps();
    if (g) {
        for (size_t i=0;i<pad_map.count;++i)
            if (SDL_GetGamepadButton(g,(SDL_GamepadButton)pad_map.entries[i].source)) d->buttons|=pad_map.entries[i].ps;
        const uint32_t mapped=d->buttons;
        d->left_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTX)); d->left_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFTY));
        d->right_x=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTX)); d->right_y=axis(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHTY));
        d->l2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_LEFT_TRIGGER)); d->r2=trigger(SDL_GetGamepadAxis(g,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        if (d->l2>30) d->buttons|=BTN_L2;
        if (d->r2>30) d->buttons|=BTN_R2;
        if (mapped & BTN_L2) d->l2=255;
        if (mapped & BTN_R2) d->r2=255;
        if (SDL_GetNumGamepadTouchpads(g)>0) {
            const int fingers=SDL_GetNumGamepadTouchpadFingers(g,0);
            for (int finger=0;finger<fingers && d->touch_count<2;++finger) {
                bool down=false;
                float x=0, y=0;
                if (SDL_GetGamepadTouchpadFinger(g,0,finger,&down,&x,&y,NULL) && down) {
                    d->touches[d->touch_count++]=(PadTouch){.x=touch_axis(x,1919),
                        .y=touch_axis(y,942),.id=(uint8_t)finger};
                }
            }
        }
        /* The prompts follow the device the player last used (runtime_button_icons.c). */
        if (slot==0 && (d->buttons || d->touch_count || stick_pushed(d))) runtime_button_icons_input(pad_icons(g));
    }
    /* The keyboard and mouse work with or without a gamepad: their buttons add to the pad's, and a
     * stick follows the keys only while one of them is held. */
    if (k && sample_keyboard(d,k,SDL_GetMouseState(NULL,NULL))) runtime_button_icons_input(BUTTON_ICONS_KEYBOARD);
}

/* Player 1's gamepad is gone (battery, cable, dropped link) while player 2's is still there: the
 * menus follow user 1, so after FAILOVER_US player 2's gamepad becomes player 1's and slot 1 waits
 * for a gamepad, which a pad that comes back then takes. A pad that returns within the grace time
 * is player 1 again (host_gamepad scans slot 0 first). Returns true when it moved. */
static bool host_failover(HostPad *pads, uint64_t *lost_since, uint64_t now) {
    if (slots()<2 || pads[0].g || !pads[1].g) { *lost_since=0; return false; }
    if (!*lost_since) { *lost_since=now; return false; }
    if (now-*lost_since<FAILOVER_US) return false;
    pads[0]=pads[1];
    pads[1]=(HostPad){.next_scan=now+100000};
    *lost_since=0;
    pthread_mutex_lock(&host_lock);
    memcpy(gamepad_name[0],gamepad_name[1],sizeof(gamepad_name[0]));
    strcpy(gamepad_name[1],"none");
    pthread_mutex_unlock(&host_lock);
    printf("Runtime: gamepad 1 lost: gamepad 2 is now player 1\n");
    return true;
}

/* Host thread: samples every millisecond (SDL_DelayNS waits on a high resolution timer) and
 * passes the game's vibration on: when it changes, and every 500 ms while the motors run, as each
 * rumble is sent for 1000 ms. */
static void *host_thread(void *arg) {
    (void)arg;
    HostPad pads[PAD_SLOTS]={{0}};
    uint64_t rumble_sent[PAD_SLOTS]={0}, lost_since=0;
    uint32_t rumble_serial[PAD_SLOTS]={0};
    for (;;) {
        const uint64_t start=now_us();
        if (!sdl_ready) sdl_ready = SDL_WasInit(SDL_INIT_GAMEPAD) ? 1 : SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
        for (int slot=0;slot<slots();++slot) {
            if (sdl_ready>0) host_gamepad(pads,slot,start);
            if (slot==1 && host_failover(pads,&lost_since,start)) rumble_serial[0]=rumble_serial[1]=0;
            PadData d;
            host_read(&d,pads[slot].g,pads[slot].connected_count,slot);
            pthread_mutex_lock(&host_lock);
            host_sample[slot]=d;
            const uint8_t large=rumble[slot].large, small=rumble[slot].small;
            const uint32_t serial=rumble[slot].serial;
            pthread_mutex_unlock(&host_lock);
            if (pads[slot].g && (serial!=rumble_serial[slot] || ((large || small) && start-rumble_sent[slot]>=500000))) {
                /* settings.ini vibration scales the game's motors (0: off). */
                const unsigned percent=(unsigned)theatrhythm_gpu_vibration_percent();
                SDL_RumbleGamepad(pads[slot].g,(uint16_t)(large*257u*percent/100u),(uint16_t)(small*257u*percent/100u),1000);
                rumble_serial[slot]=serial; rumble_sent[slot]=start;
            }
        }
        note_call(SLOW_HOST,start);
        SDL_DelayNS(1000000);
    }
    return NULL;
}

/* THEATRHYTHM_PAD_FILE=<file>: scripted input for automated runs. The file holds whitespace-separated
 * tokens, re-read when it changes: button names (cross circle square triangle l1 r1 l2 r2 l3 r3
 * options touchpad up down left right) are held while listed; lx= ly= rx= ry= (0..255) override
 * the sticks; device=ps4, device=xbox or device=keyboard reports that device as the one in use (the
 * prompt icons follow it); reconnect closes the gamepad once per edit and the host thread opens
 * it again, as after a real reconnect. An empty file releases everything. */
static struct { uint32_t buttons; int stick[4], device; } injected={0,{-1,-1,-1,-1},-1};
static int replay_armed;      /* 1 while a THEATRHYTHM_PAD_REPLAY recording plays, 2 once it ended */
static uint64_t replay_start; /* 0: (re)start at the next sample */
static void read_inject(void) {
    static const char *path; static int checked; static uint64_t last_check; static struct timespec mtime;
    if (!checked) { path=getenv("THEATRHYTHM_PAD_FILE"); checked=1; }
    if (!path || !*path) return;
    uint64_t now=now_us();
    if (now-last_check<20000) return;
    last_check=now;
    struct stat st;
    if (stat(path,&st)!=0) return;
#ifdef _WIN32
    /* Whole-second mtimes: the size tells two edits within a second apart. */
    if (st.st_mtime==mtime.tv_sec && st.st_size==mtime.tv_nsec) return;
    mtime=(struct timespec){st.st_mtime,(long)st.st_size};
#else
    if (st.st_mtim.tv_sec==mtime.tv_sec && st.st_mtim.tv_nsec==mtime.tv_nsec) return;
    mtime=st.st_mtim;
#endif
    FILE *f=fopen(path,"r");
    if (!f) return;
    static const char *sticks[]={"lx=","ly=","rx=","ry="};
    static const char *devices[BUTTON_ICONS_SETS]={"device=ps4","device=xbox","device=keyboard"};
    injected.buttons=0;
    injected.device=-1;
    for (int i=0;i<4;++i) injected.stick[i]=-1;
    char token[64];
    int reconnect=0;
    while (fscanf(f,"%63s",token)==1) {
        if (!strcmp(token,"replay") && replay_armed!=1) { replay_armed=1; replay_start=0; } /* THEATRHYTHM_PAD_REPLAY */
        if (!strcmp(token,"reconnect")) reconnect=1;
        for (size_t i=0;i<sizeof(ps_names)/sizeof(*ps_names);++i) if (!strcmp(token,ps_names[i].name)) injected.buttons|=ps_names[i].ps;
        for (int i=0;i<4;++i) if (!strncmp(token,sticks[i],3)) { int v=atoi(token+3); injected.stick[i]=v<0 ? 0 : v>255 ? 255 : v; }
        for (int i=0;i<BUTTON_ICONS_SETS;++i) if (!strcmp(token,devices[i])) injected.device=i;
    }
    fclose(f);
    if (reconnect) for (int slot=0;slot<PAD_SLOTS;++slot) __atomic_store_n(&reconnect_wanted[slot],1,__ATOMIC_RELEASE); /* host_gamepad closes it */
    printf("Runtime: pad file: buttons 0x%x sticks %d %d %d %d device %d\n",injected.buttons,
           injected.stick[0],injected.stick[1],injected.stick[2],injected.stick[3],injected.device);
}
/* THEATRHYTHM_PAD_RECORD=<file>: F9 starts and stops recording the pad state (gamepad or keyboard) with
 * the time since F9; THEATRHYTHM_PAD_REPLAY=<file> plays such a recording back, started by the token
 * "replay" in THEATRHYTHM_PAD_FILE (scripted tests repeat a route the player ran once). Lines: ms buttons
 * lx ly rx ry l2 r2, written when the state changes. */
typedef struct { uint32_t ms, buttons; uint8_t axes[4], l2, r2; } PadSample;
static FILE *record_file;
static uint64_t record_start;
static PadSample record_last;
static void record_sample(const PadData *d) {
    static const char *path; static int checked, f9_was_down;
    if (!checked) { path=getenv("THEATRHYTHM_PAD_RECORD"); checked=1; }
    if (!path || !*path || !sdl_ready) return;
    const bool *k=SDL_GetKeyboardState(NULL);
    const int f9=k && k[SDL_SCANCODE_F9];
    if (f9 && !f9_was_down) {
        if (record_file) {
            fclose(record_file); record_file=NULL;
            printf("Runtime: pad recording stopped (%s)\n",path);
        } else if ((record_file=fopen(path,"w"))) {
            record_start=now_us();
            memset(&record_last,0xff,sizeof(record_last));
            printf("Runtime: pad recording started (%s, F9 stops)\n",path);
        }
    }
    f9_was_down=f9;
    if (!record_file) return;
    PadSample s={(uint32_t)((now_us()-record_start)/1000),d->buttons,
                 {d->left_x,d->left_y,d->right_x,d->right_y},d->l2,d->r2};
    if (s.buttons==record_last.buttons && !memcmp(s.axes,record_last.axes,4) &&
        s.l2==record_last.l2 && s.r2==record_last.r2) return;
    record_last=s;
    fprintf(record_file,"%u %u %u %u %u %u %u %u\n",s.ms,s.buttons,s.axes[0],s.axes[1],s.axes[2],
            s.axes[3],s.l2,s.r2);
    fflush(record_file);
}
static PadSample *replay; static size_t replay_count, replay_next;
static void replay_sample(PadData *d) {
    if (!replay_armed) return;
    if (!replay_start) {
        static int loaded;
        if (!loaded) {
            loaded=1;
            const char *path=getenv("THEATRHYTHM_PAD_REPLAY");
            FILE *f=path ? fopen(path,"r") : NULL;
            PadSample s; unsigned v[8]; size_t cap=0;
            while (f && fscanf(f,"%u %u %u %u %u %u %u %u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8) {
                s=(PadSample){v[0],v[1],{(uint8_t)v[2],(uint8_t)v[3],(uint8_t)v[4],(uint8_t)v[5]},(uint8_t)v[6],(uint8_t)v[7]};
                if (replay_count==cap && !(replay=realloc(replay,(cap=cap ? cap*2 : 1024)*sizeof(*replay)))) break;
                replay[replay_count++]=s;
            }
            if (f) fclose(f);
            printf("Runtime: pad replay of %zu samples from %s\n",replay_count,path ? path : "(unset)");
        }
        replay_start=now_us();
        replay_next=0;
    }
    const uint32_t ms=(uint32_t)((now_us()-replay_start)/1000);
    while (replay_next<replay_count && replay[replay_next].ms<=ms) ++replay_next;
    if (!replay_next) return;
    if (replay_next==replay_count && ms>replay[replay_count-1].ms+500) {
        if (replay_armed==1) { puts("Runtime: pad replay finished"); replay_armed=2; }
        return;
    }
    const PadSample *s=&replay[replay_next-1];
    d->buttons=s->buttons;
    d->left_x=s->axes[0]; d->left_y=s->axes[1]; d->right_x=s->axes[2]; d->right_y=s->axes[3];
    d->l2=s->l2; d->r2=s->r2;
}

/* F4's exit window (runtime_exit_prompt.c) takes only Left/Right between No (left, focused first)
 * and Yes, and Cross; Circle does nothing there. So Circle and Options (Escape is Circle; Start,
 * Tab and P are Options) answer No as a player would: Left, which stays on No, then Cross, each
 * held 80 ms with a gap. Meanwhile the pad shows only that, and the key that started it, with the
 * Left and Cross, stays hidden until released, so nothing reaches what is under the window. */
enum { NO_STEP_US=80000 };
static struct { uint64_t start; int active, held; } prompt_no;
static void exit_prompt_no(PadData *d) {
    const uint32_t keys=BTN_CIRCLE|BTN_OPTIONS, played=keys|BTN_LEFT|BTN_CROSS;
    const int open=runtime_exit_prompt_open();
    if (prompt_no.active && open) {
        const uint64_t t=now_us()-prompt_no.start;
        d->buttons=t<NO_STEP_US ? BTN_LEFT : t>=2*NO_STEP_US && t<3*NO_STEP_US ? BTN_CROSS : 0;
        if (t>=4*NO_STEP_US) prompt_no.active=0; /* the window is still open: the key may try again */
        return;
    }
    prompt_no.active=0;
    if (prompt_no.held) {
        if (d->buttons & (open ? keys : played)) { d->buttons&=~(open ? keys : played); return; }
        prompt_no.held=0;
    }
    if (open && (d->buttons & keys)) {
        prompt_no.active=prompt_no.held=1;
        prompt_no.start=now_us();
        d->buttons=BTN_LEFT;
    }
}

/* After a dialog closes, buttons still held (the Cross that accepted a name) stay hidden until
 * released: the game would take them as a new press. */
static int hold_after_capture[PAD_SLOTS];
/* Player 1 also gets the recording, replay and scripted inputs, and the exit window's No. */
static void sample(PadData *d, int slot) {
    pthread_mutex_lock(&host_lock);
    *d=host_sample[slot];
    pthread_mutex_unlock(&host_lock);
    if (!d->connected) { /* before the host thread's first sample */
        d->left_x=d->left_y=d->right_x=d->right_y=128;
        d->orientation[3]=1.0f;
        d->connected=d->connected_count=1;
    }
    d->timestamp=now_us();
    if (theatrhythm_gpu_overlay_captures_input()) { hold_after_capture[slot]=1; click[slot].active=false; return; }
    if (slot==0) {
        record_sample(d); /* the click sources as they are: a replay plays them again */
        read_inject();
        if (injected.device>=0) runtime_button_icons_input(injected.device);
        replay_sample(d);
        d->buttons|=injected.buttons;
        if (injected.buttons & BTN_L2) d->l2=255;
        if (injected.buttons & BTN_R2) d->r2=255;
        uint8_t *axes[4]={&d->left_x,&d->left_y,&d->right_x,&d->right_y};
        for (int i=0;i<4;++i) if (injected.stick[i]>=0) *axes[i]=(uint8_t)injected.stick[i];
    }
    play_click(d,slot);
    if (slot==0) exit_prompt_no(d);
    else if (runtime_exit_prompt_open()) d->buttons=0; /* the window is player 1's */
    if (hold_after_capture[slot]) {
        if (d->buttons) d->buttons=0;
        else hold_after_capture[slot]=0;
    }
}

static void start_host_thread(void) {
    pthread_t thread;
    if (pthread_create(&thread,NULL,host_thread,NULL)==0) pthread_detach(thread);
    else puts("Runtime: pad host thread failed to start; the pad reads neutral");
}
static ABI int32_t pad_init(void) { pthread_mutex_lock(&lock); initialized=1; pthread_mutex_unlock(&lock); return 0; }
static ABI int32_t pad_open(int32_t user, int32_t type, int32_t index, const void *param) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user<1 || user>slots()) return ERR_INVALID_ARG; /* user n is pad slot n-1 */
    if (type!=0 && type!=2) return ERR_INVALID_ARG; /* standard / special port */
    if (index) return ERR_INVALID_ARG;
    const int slot=user-1;
    pthread_mutex_lock(&lock);
    int already=opened[slot]; opened[slot]=1;
    pthread_mutex_unlock(&lock);
    if (already) return ERR_ALREADY_OPENED;
    static pthread_once_t host_started=PTHREAD_ONCE_INIT;
    pthread_once(&host_started,start_host_thread);
    printf("Runtime: pad opened for user %d (SDL gamepad%s)\n",user,slot ? "" : " or keyboard");
    return PAD_HANDLE+slot;
}
static ABI int32_t pad_close(int32_t handle) {
    const int slot=slot_of(handle);
    if (slot<0) return ERR_INVALID_HANDLE;
    opened[slot]=0; return 0;
}
static ABI int32_t pad_read_state(int32_t handle, PadData *data) {
    const int slot=slot_of(handle);
    if (slot<0) return ERR_INVALID_HANDLE;
    if (!data) return ERR_INVALID_ARG;
    const uint64_t start=now_us();
    pthread_mutex_lock(&lock);
    sample(data,slot); ++reads;
    note_call(SLOW_READ,start);
    pthread_mutex_unlock(&lock);
    return 0;
}
/* Buffered read: the port samples once per call, so one entry is returned. */
static ABI int32_t pad_read(int32_t handle, PadData *data, int32_t count) {
    if (slot_of(handle)<0) return ERR_INVALID_HANDLE;
    if (!data || count<1 || count>64) return ERR_INVALID_ARG;
    pad_read_state(handle,data);
    return 1;
}
static ABI int32_t pad_info(int32_t handle, ControllerInfo *info) {
    const int slot=slot_of(handle);
    if (slot<0) return ERR_INVALID_HANDLE;
    if (!info) return ERR_INVALID_ARG;
    memset(info,0,sizeof(*info));
    info->pixel_density=44.86f; info->resolution_x=1920; info->resolution_y=943;
    info->dead_zone_left=info->dead_zone_right=2;
    info->connection_type=0; info->connected=1; info->device_class=0;
    const uint64_t start=now_us();
    pthread_mutex_lock(&host_lock);
    info->connected_count=host_sample[slot].connected_count ? host_sample[slot].connected_count : 1;
    pthread_mutex_unlock(&host_lock);
    note_call(SLOW_INFO,start);
    return 0;
}
static ABI int32_t pad_vibration(int32_t handle, const uint8_t *param) {
    const int slot=slot_of(handle);
    if (slot<0) return ERR_INVALID_HANDLE;
    if (!param) return ERR_INVALID_ARG;
    const uint64_t start=now_us();
    pthread_mutex_lock(&host_lock); /* host_thread sends it */
    if (rumble[slot].large!=param[0] || rumble[slot].small!=param[1]) { rumble[slot].large=param[0]; rumble[slot].small=param[1]; ++rumble[slot].serial; }
    pthread_mutex_unlock(&host_lock);
    note_call(SLOW_VIBRATION,start);
    __atomic_add_fetch(&vibrations,1,__ATOMIC_RELAXED);
    return 0;
}
static ABI int32_t pad_ok_handle(int32_t handle) { return slot_of(handle)>=0 ? 0 : ERR_INVALID_HANDLE; }
static ABI int32_t pad_ok_handle_flag(int32_t handle, uint8_t flag) { (void)flag; return pad_ok_handle(handle); }

/* scePadDeviceClassGetExtendedInformation(handle, info): a standard controller, class 0 and no
 * class data (20 bytes zeroed). scePadDeviceClassParseData has nothing to parse for it and leaves
 * the output alone, as shadPS4 does. */
static ABI int32_t pad_class_info(int32_t handle, uint8_t *info) {
    if (slot_of(handle)<0) return ERR_INVALID_HANDLE;
    if (info) memset(info,0,20);
    return 0;
}
static ABI int32_t pad_class_parse(int32_t handle, const PadData *data, void *out) {
    (void)data; (void)out; return pad_ok_handle(handle);
}
static ABI int32_t pad_light_bar(int32_t handle, const uint8_t *rgb) {
    if (slot_of(handle)<0) return ERR_INVALID_HANDLE;
    (void)rgb; return 0;
}
static const RuntimeExport exports[]={
    {"scePadSetLightBar",pad_light_bar}, {"scePadResetLightBar",pad_ok_handle},
    {"scePadInit",pad_init}, {"scePadOpen",pad_open}, {"scePadClose",pad_close},
    {"scePadReadState",pad_read_state}, {"scePadRead",pad_read},
    {"scePadGetControllerInformation",pad_info}, {"scePadSetVibration",pad_vibration},
    {"scePadResetOrientation",pad_ok_handle},
    {"scePadSetAngularVelocityDeadbandState",pad_ok_handle_flag}, {"scePadSetTiltCorrectionState",pad_ok_handle_flag},
    {"scePadSetMotionSensorState",pad_ok_handle_flag},
    {"scePadDeviceClassGetExtendedInformation",pad_class_info}, {"scePadDeviceClassParseData",pad_class_parse},
};
uintptr_t runtime_pad_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_pad_report(void) {
    char name[PAD_SLOTS*(sizeof(gamepad_name[0])+2)];
    pthread_mutex_lock(&host_lock);
    snprintf(name,sizeof(name),"%s%s%s",gamepad_name[0],slots()>1 ? ", " : "",slots()>1 ? gamepad_name[1] : "");
    pthread_mutex_unlock(&host_lock);
    printf("Runtime: pad reads=%zu, vibrations=%zu, gamepads=%s; over 2 ms: reads %zu (max %.1f ms), "
           "vibrations %zu (max %.1f ms), info %zu (max %.1f ms), host samples %zu (max %.1f ms)\n",reads,vibrations,name,
           slow_calls[SLOW_READ],slow_max_us[SLOW_READ]/1e3,slow_calls[SLOW_VIBRATION],slow_max_us[SLOW_VIBRATION]/1e3,
           slow_calls[SLOW_INFO],slow_max_us[SLOW_INFO]/1e3,slow_calls[SLOW_HOST],slow_max_us[SLOW_HOST]/1e3);
}

/* probe --gamepad: runs before any game state exists, so it uses its own SDL gamepad and leaves
 * the lock and statics above alone. */
static const char *label_name(SDL_GamepadButtonLabel label) {
    switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: return "a";
    case SDL_GAMEPAD_BUTTON_LABEL_B: return "b";
    case SDL_GAMEPAD_BUTTON_LABEL_X: return "x";
    case SDL_GAMEPAD_BUTTON_LABEL_Y: return "y";
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return "cross";
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return "circle";
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return "square";
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "triangle";
    default: return "unknown";
    }
}
int runtime_pad_describe(void) {
    SDL_Gamepad *g=NULL;
    const bool ready=SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    /* No event loop yet, so pads show up only after a pump: poll for about 300 ms. */
    for (int waited=0; ready && !g && waited<30; ++waited) {
        SDL_PumpEvents();
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        if (ids && count>0) g=SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
        if (!g) SDL_Delay(10);
    }
    if (!g) puts("Gamepad: none");
    else {
        const char *type=SDL_GetGamepadStringForType(SDL_GetGamepadType(g));
        const char *name=SDL_GetGamepadName(g);
        printf("Gamepad: type=%s south=%s east=%s west=%s north=%s name=%s\n",
               type ? type : "unknown",
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_SOUTH)),
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_EAST)),
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_WEST)),
               label_name(SDL_GetGamepadButtonLabel(g,SDL_GAMEPAD_BUTTON_NORTH)),
               name ? name : "unknown");
        SDL_CloseGamepad(g);
    }
    if (ready) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    return g!=NULL;
}
