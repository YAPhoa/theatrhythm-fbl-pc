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
static int stub_players=1;
int theatrhythm_gpu_players(void) { return stub_players; }
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
    host_sample[0]=(PadData){.connected=1,.connected_count=1,.buttons=BTN_OPTIONS|BTN_UP};
    sample(&d,0);
    assert(d.buttons==(BTN_OPTIONS|BTN_UP));
    prompt_open=1;
    sample(&d,0);
    assert(d.buttons==BTN_LEFT);
    SDL_Delay(120); sample(&d,0);
    assert(d.buttons==0);
    SDL_Delay(80); sample(&d,0);
    assert(d.buttons==BTN_CROSS);
    prompt_open=0; /* the window took the Cross */
    sample(&d,0);
    assert(d.buttons==BTN_UP); /* Options still held: hidden; Up goes on */
    host_sample[0].buttons=BTN_UP|BTN_CROSS;
    sample(&d,0);
    assert(d.buttons==BTN_UP); /* a Cross held then too, until all are released */
    host_sample[0].buttons=BTN_UP;
    sample(&d,0);
    assert(d.buttons==BTN_UP);
    host_sample[0].buttons=BTN_CIRCLE; /* Circle outside the prompt is Circle */
    sample(&d,0);
    assert(d.buttons==BTN_CIRCLE);
    host_sample[0]=(PadData){0};
}

/* Two players: user 2 is pad slot 1 (handle 2), only with players=2; the scripted inputs and the
 * exit window's No stay with player 1. */
static void test_second_player(void) {
    PadData d;
    initialized=1;
    stub_players=1;
    assert(pad_open(2,0,0,NULL)==ERR_INVALID_ARG && !opened[1]);
    stub_players=2;
    assert(pad_open(0,0,0,NULL)==ERR_INVALID_ARG && pad_open(3,0,0,NULL)==ERR_INVALID_ARG && pad_open(2,0,1,NULL)==ERR_INVALID_ARG);
    assert(slot_of(1)<0 && slot_of(2)<0 && pad_read_state(2,&d)==ERR_INVALID_HANDLE);
    opened[0]=opened[1]=1; /* as pad_open does, without starting the host thread */
    assert(slot_of(1)==0 && slot_of(2)==1 && slot_of(3)<0 && pad_ok_handle(2)==0);
    assert(pad_open(2,0,0,NULL)==ERR_ALREADY_OPENED);
    host_sample[0]=(PadData){.connected=1,.connected_count=1,.buttons=BTN_UP};
    host_sample[1]=(PadData){.connected=1,.connected_count=2,.buttons=BTN_DOWN};
    assert(pad_read_state(1,&d)==0 && d.buttons==BTN_UP);
    assert(pad_read_state(2,&d)==0 && (d.buttons & BTN_DOWN) && !(d.buttons & BTN_UP));
    ControllerInfo info;
    assert(pad_info(2,&info)==0 && info.connected_count==2 && pad_info(1,&info)==0 && info.connected_count==1);
    /* Rumble is per user. */
    const uint8_t motors[2]={200,100};
    assert(pad_vibration(2,motors)==0 && rumble[1].large==200 && rumble[0].large==0);
    /* The exit window belongs to player 1: player 2's buttons are hidden while it is open. */
    host_sample[1].buttons=BTN_CROSS;
    prompt_open=1;
    assert(pad_read_state(2,&d)==0 && d.buttons==0);
    prompt_open=0;
    assert(pad_read_state(2,&d)==0 && d.buttons==BTN_CROSS);
    assert(pad_close(2)==0 && !opened[1] && pad_close(2)==ERR_INVALID_HANDLE && opened[0]);
    opened[0]=0; stub_players=1;
    host_sample[0]=host_sample[1]=(PadData){0};
    memset(&rumble,0,sizeof(rumble));
}

/* players=auto with no gamepad connected plays with one, and the answer is kept. */
static void test_auto_players(void) {
    stub_players=0;
    assert(runtime_players()==1 && slots()==1);
    stub_players=2;
    assert(runtime_players()==2); /* a fixed setting always wins */
    stub_players=0;
    assert(runtime_players()==1); /* decided once */
    stub_players=1;
}

/* Player 1's gamepad drops: after the grace time player 2's takes its place; one that comes back
 * sooner is player 1 again, and a one-player run never moves anything. */
static void test_failover(void) {
    SDL_Gamepad *const a=(SDL_Gamepad *)0x1000, *const b=(SDL_Gamepad *)0x2000;
    HostPad pads[PAD_SLOTS]={{0}};
    uint64_t lost=0, t=1000000;
    stub_players=2;
    pads[0].g=a; pads[1].g=b; pads[1].id=7; pads[1].connected_count=2;
    assert(!host_failover(pads,&lost,t) && lost==0);            /* both there */
    pads[0].g=NULL;
    assert(!host_failover(pads,&lost,t) && lost==t);            /* starts the clock */
    assert(!host_failover(pads,&lost,t+FAILOVER_US-1) && pads[1].g==b);
    pads[0].g=a;                                                /* it came back in time */
    assert(!host_failover(pads,&lost,t+FAILOVER_US) && lost==0 && pads[0].g==a && pads[1].g==b);
    pads[0].g=NULL; t+=10000000;
    assert(!host_failover(pads,&lost,t) && !host_failover(pads,&lost,t+FAILOVER_US-1));
    strcpy(gamepad_name[1],"Pad B");
    assert(host_failover(pads,&lost,t+FAILOVER_US));            /* moved */
    assert(pads[0].g==b && pads[0].id==7 && pads[0].connected_count==2 && !pads[1].g && lost==0);
    assert(!strcmp(gamepad_name[0],"Pad B") && !strcmp(gamepad_name[1],"none"));
    assert(!host_failover(pads,&lost,t+2*FAILOVER_US));         /* nothing left to move */
    pads[1].g=a;                                                /* player 1's old pad returns as player 2 */
    assert(!host_failover(pads,&lost,t+3*FAILOVER_US) && pads[0].g==b && pads[1].g==a);
    pads[0].g=NULL; pads[1].g=NULL; lost=0;
    assert(!host_failover(pads,&lost,t) && lost==0);            /* no pads: nothing to move */
    stub_players=1; pads[0].g=NULL; pads[1].g=b;
    assert(!host_failover(pads,&lost,t) && !host_failover(pads,&lost,t+9*FAILOVER_US) && !pads[0].g);
    strcpy(gamepad_name[0],"none"); strcpy(gamepad_name[1],"none");
}

int main(void) {
    test_default_keyboard();
    test_key_map();
    test_pad_map();
    test_exit_prompt_no();
    test_second_player();
    test_auto_players();
    test_failover();
    puts("pad tests passed");
    return 0;
}
