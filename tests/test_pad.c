/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Pad mapping: the default keyboard layout, key_map/pad_map parsing and the mouse sources. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/runtime_pad.c"

int theatrhythm_gpu_overlay_captures_input(void) { return 0; }
int theatrhythm_gpu_vibration_percent(void) { return 100; }
const char *theatrhythm_pad_map(int keyboard) { (void)keyboard; return ""; }
void runtime_button_icons_input(int set) { (void)set; }
static int prompt_open;
int runtime_exit_prompt_open(void) { return prompt_open; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static bool keys[SDL_SCANCODE_COUNT];

/* The keyboard's contribution to a neutral pad with the keys in `held` down. */
static PadData press(const SDL_Scancode *held, size_t count, uint32_t mouse) {
    memset(keys,0,sizeof(keys));
    for (size_t i=0;i<count;++i) keys[held[i]]=true;
    PadData d;
    memset(&d,0,sizeof(d));
    d.left_x=d.left_y=d.right_x=d.right_y=128;
    sample_keyboard(&d,keys,mouse);
    return d;
}
static PadData press1(SDL_Scancode key) { return press(&key,1,0); }

static size_t sources_of(const ButtonMap *map, uint32_t ps) {
    size_t n=0;
    for (size_t i=0;i<map->count;++i) n+=map->entries[i].ps==ps;
    return n;
}

static void test_default_keyboard(void) {
    assert(press1(SDL_SCANCODE_SPACE).buttons==BTN_CROSS);
    assert(press1(SDL_SCANCODE_RETURN).buttons==BTN_CROSS);
    assert(press1(SDL_SCANCODE_ESCAPE).buttons==BTN_CIRCLE);
    assert(press1(SDL_SCANCODE_F).buttons==BTN_SQUARE);
    assert(press1(SDL_SCANCODE_H).buttons==BTN_TRIANGLE);
    assert(press1(SDL_SCANCODE_Q).buttons==BTN_L1 && press1(SDL_SCANCODE_O).buttons==BTN_R1);
    assert(press1(SDL_SCANCODE_UP).buttons==BTN_UP && press1(SDL_SCANCODE_RIGHT).buttons==BTN_RIGHT);
    assert(press1(SDL_SCANCODE_TAB).buttons==BTN_OPTIONS);
    assert(press1(SDL_SCANCODE_T).buttons==CLICK); /* play_click turns it into a touch */
    PadData d=press1(SDL_SCANCODE_E);
    assert(d.buttons==BTN_L2 && d.l2==255);
    /* Sticks: no button bits, full tilt, both sticks at once (dual slides). */
    const SDL_Scancode slide[]={SDL_SCANCODE_W,SDL_SCANCODE_D,SDL_SCANCODE_J};
    d=press(slide,3,0);
    assert(d.buttons==0);
    assert(d.left_y==0 && d.left_x==255 && d.right_x==0 && d.right_y==128);
    /* Two taps at once (dual triggers) from either hand. */
    const SDL_Scancode taps[]={SDL_SCANCODE_F,SDL_SCANCODE_H};
    assert(press(taps,2,0).buttons==(BTN_SQUARE|BTN_TRIANGLE));
    assert(press(NULL,0,SDL_BUTTON_LMASK).buttons==0); /* no mouse source by default */
}

static void test_key_map(void) {
    const ButtonMap saved=key_map;
    /* The first mention replaces the defaults, the next ones add; mouse buttons are sources. */
    assert(apply_button_map(&key_map,"cross=J, cross=Mouse Left ,square=Left Shift",true)==3);
    assert(sources_of(&key_map,BTN_CROSS)==2 && sources_of(&key_map,BTN_SQUARE)==1);
    assert(press1(SDL_SCANCODE_SPACE).buttons==0);
    assert(press1(SDL_SCANCODE_J).buttons==BTN_CROSS && press1(SDL_SCANCODE_J).right_x==0);
    assert(press(NULL,0,SDL_BUTTON_LMASK).buttons==BTN_CROSS);
    assert(press(NULL,0,SDL_BUTTON_RMASK).buttons==0);
    assert(press1(SDL_SCANCODE_LSHIFT).buttons==BTN_SQUARE && press1(SDL_SCANCODE_F).buttons==0);
    /* None empties a button; bad entries leave the defaults alone; stick directions map. */
    assert(apply_button_map(&key_map,"options=None,bogus=Q,circle=NotAKey,touchpad,rstick_up=Up",true)==2);
    assert(sources_of(&key_map,BTN_OPTIONS)==0 && sources_of(&key_map,BTN_CIRCLE)==3);
    assert(press1(SDL_SCANCODE_UP).right_y==0);
    assert(apply_button_map(&key_map,"l1=Comma,r1=Equals",true)==2);
    assert(press1(SDL_SCANCODE_COMMA).buttons==BTN_L1 && press1(SDL_SCANCODE_EQUALS).buttons==BTN_R1);
    key_map=saved;
}

static void test_pad_map(void) {
    const ButtonMap saved=pad_map;
    assert(apply_button_map(&pad_map,"cross=b,circle=a,lstick_up=dpup",false)==2);
    assert(sources_of(&pad_map,BTN_CROSS)==1 && sources_of(&pad_map,BTN_CIRCLE)==1);
    for (size_t i=0;i<pad_map.count;++i) {
        if (pad_map.entries[i].ps==BTN_CROSS) assert(pad_map.entries[i].source==SDL_GAMEPAD_BUTTON_EAST);
        if (pad_map.entries[i].ps==BTN_CIRCLE) assert(pad_map.entries[i].source==SDL_GAMEPAD_BUTTON_SOUTH);
    }
    assert(sources_of(&pad_map,CLICK)==1); /* Back/View: misc1 is a DualSense's mute button */
    pad_map=saved;
}

/* While the exit prompt is open, Options (or Circle) plays Left, then Cross (No); the key stays
 * hidden until released, also after the window closed. Outside the prompt it passes as it is. */
static void test_exit_prompt_no(void) {
    PadData d;
    host_sample=(PadData){.connected=1,.connected_count=1,.buttons=BTN_OPTIONS|BTN_UP};
    sample(&d);
    assert(d.buttons==(BTN_OPTIONS|BTN_UP));
    prompt_open=1;
    sample(&d);
    assert(d.buttons==BTN_LEFT);
    SDL_Delay(120); sample(&d);
    assert(d.buttons==0);
    SDL_Delay(80); sample(&d);
    assert(d.buttons==BTN_CROSS);
    prompt_open=0; /* the window took the Cross */
    sample(&d);
    assert(d.buttons==BTN_UP); /* Options still held: hidden; Up goes on */
    host_sample.buttons=BTN_UP|BTN_CROSS;
    sample(&d);
    assert(d.buttons==BTN_UP); /* a Cross held then too, until all are released */
    host_sample.buttons=BTN_UP;
    sample(&d);
    assert(d.buttons==BTN_UP);
    host_sample.buttons=BTN_CIRCLE; /* Circle outside the prompt is Circle */
    sample(&d);
    assert(d.buttons==BTN_CIRCLE);
    host_sample=(PadData){0};
}

int main(void) {
    test_default_keyboard();
    test_key_map();
    test_pad_map();
    test_exit_prompt_no();
    puts("pad tests passed");
    return 0;
}
