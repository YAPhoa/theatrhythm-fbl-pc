/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Button prompt icons and the Live Info pad (src/runtime_button_icons.c): a fixed set mounts its
 * bundles; auto finds the atlases in (fake) guest memory, rewrites them for the device last used and
 * moves the mounts with them. The pad has no keyboard drawing: keyboard shows the DualShock 4. */
#define _GNU_SOURCE
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#include "../src/runtime_button_icons.c"
#include <assert.h>
#include <sys/stat.h>
#include <time.h>

/* Guest memory: one mapped range at GUEST, host bytes in `memory`. */
#define GUEST UINT64_C(0x2000000000)
#define MEMORY (1u<<20)
#define ATLAS UINT64_C(0x40100) /* 256-aligned offset of the loaded atlas */
#define PAD_ATLAS UINT64_C(0xc0000) /* and of the pad atlas (to the end of memory) */
static unsigned char memory[MEMORY];
static uintptr_t invalidated; static uint64_t invalidated_size; static int invalidations;
void runtime_memory_visit_mapped(void (*visit)(uintptr_t, const unsigned char *, uint64_t, void *), void *context) {
    visit(GUEST,memory,MEMORY,context);
}
void runtime_memory_read_backing(uintptr_t address, void *data, uint64_t size) { memcpy(data,memory+(address-GUEST),size); }
int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size) {
    memcpy(memory+(address-GUEST),data,size); return 1;
}
static int pad_invalidations;
void runtime_memory_invalidate(uintptr_t address, uint64_t size) {
    if (address==GUEST+PAD_ATLAS) { assert(size==PAD_BYTES); ++pad_invalidations; return; }
    invalidated=address; invalidated_size=size; ++invalidations;
}

/* File mounts: the host file each of the two guest paths serves ("" for the game's own). */
static char mounted[2][600], pad_mounted[2][600];
static int slot(const char *guest) { return strncmp(guest,"/app0/",6) ? 1 : 0; }
static char *mount_of(const char *guest) {
    if (strstr(guest,atlases[PAD].bundle)) return pad_mounted[slot(guest)];
    assert(strstr(guest,atlases[ICONS].bundle));
    return mounted[slot(guest)];
}
int runtime_file_mount(const char *guest, const char *host) { snprintf(mount_of(guest),600,"%s",host); return 0; }
void runtime_file_unmount(const char *guest) { mount_of(guest)[0]=0; }

static const char *dir="out/button-icons-scratch", *pad_dir="out/button-icons-scratch/pad";
static unsigned char sets[BUTTON_ICONS_SETS][TEXTURE_BYTES], pads[2][PAD_BYTES];
static void write_in(const char *folder, const char *name, const void *data, size_t size) {
    char path[256]; snprintf(path,sizeof(path),"%s/%s",folder,name);
    FILE *f=fopen(path,"wb"); assert(f); assert(fwrite(data,1,size,f)==size); assert(!fclose(f));
}
static void write_file(const char *name, const void *data, size_t size) { write_in(dir,name,data,size); }
static void expect_pad(int set) {
    char want[600]="";
    if (set==BUTTON_ICONS_XBOX) snprintf(want,sizeof(want),"%s/xbox.bundle",pad_dir);
    assert(!strcmp(pad_mounted[0],want) && !strcmp(pad_mounted[1],want));
    assert(!memcmp(memory+PAD_ATLAS,pads[set==BUTTON_ICONS_XBOX],PAD_BYTES));
    assert(runtime_button_icons_pad()==set);
}
static void expect_mount(const char *name) {
    char want[600]="";
    if (name) snprintf(want,sizeof(want),"%s/%s.bundle",dir,name);
    assert(!strcmp(mounted[0],want) && !strcmp(mounted[1],want));
}
/* Reports a device and waits for the worker to finish the switch. */
static void use(int set) {
    runtime_button_icons_input(set);
    pthread_mutex_lock(&lock);
    while (shown!=set) pthread_cond_wait(&wake,&lock);
    pthread_mutex_unlock(&lock);
}

int main(void) {
    mkdir("out",0755); mkdir(dir,0755); mkdir(pad_dir,0755);
    /* Three sets alike except here and there, as icon sheets are (transparent areas match). */
    for (int set=0;set<BUTTON_ICONS_SETS;++set) {
        for (size_t i=0;i<TEXTURE_BYTES;++i) sets[set][i]=(unsigned char)(i*31u>>3);
        for (size_t i=4096;i<TEXTURE_BYTES;i+=4096) sets[set][i]^=(unsigned char)(set*0x55+1);
        static const char *names[]={"ps4.texture","xbox.texture","keyboard.texture"};
        write_file(names[set],sets[set],TEXTURE_BYTES);
    }
    write_file("xbox.bundle","x",1);
    write_file("keyboard.bundle","k",1);
    for (int set=0;set<2;++set) {
        for (size_t i=0;i<PAD_BYTES;++i) pads[set][i]=(unsigned char)(i*13u>>2);
        for (size_t i=8192;i<PAD_BYTES;i+=8192) pads[set][i]^=(unsigned char)(set*0x33+7);
    }
    write_in(pad_dir,"ps4.texture",pads[0],PAD_BYTES);
    write_in(pad_dir,"xbox.texture",pads[1],PAD_BYTES);
    write_in(pad_dir,"xbox.bundle","p",1);

    /* A fixed set only mounts its bundles (the pad has none for keyboard). */
    runtime_button_icons_configure(dir,pad_dir,"keyboard");
    expect_mount("keyboard");
    assert(!automatic && !pad_mounted[0][0] && runtime_button_icons_pad()==BUTTON_ICONS_PS4);
    memset(mounted,0,sizeof(mounted)); memset(pad_mounted,0,sizeof(pad_mounted));
    runtime_button_icons_configure(dir,pad_dir,"xbox");
    expect_mount("xbox");
    assert(!strcmp(pad_mounted[0],"out/button-icons-scratch/pad/xbox.bundle") && runtime_button_icons_pad()==BUTTON_ICONS_XBOX);
    memset(mounted,0,sizeof(mounted)); memset(pad_mounted,0,sizeof(pad_mounted));
    runtime_button_icons_configure(dir,pad_dir,"ps4");
    expect_mount(NULL);

    /* auto: the game loaded its own atlas; a copy off the texture alignment is not one. */
    for (size_t i=0;i<MEMORY;++i) memory[i]=(unsigned char)(i*7u);
    memcpy(memory+ATLAS,sets[BUTTON_ICONS_PS4],TEXTURE_BYTES);
    memcpy(memory+ATLAS+TEXTURE_BYTES+64,sets[BUTTON_ICONS_PS4],TEXTURE_BYTES);
    memcpy(memory+PAD_ATLAS,pads[0],PAD_BYTES);
    runtime_button_icons_configure(dir,pad_dir,"auto");
    assert(automatic);
    runtime_button_icons_input(BUTTON_ICONS_PS4); /* already shown: nothing to do */
    assert(!invalidations);

    use(BUTTON_ICONS_XBOX);
    assert(!memcmp(memory+ATLAS,sets[BUTTON_ICONS_XBOX],TEXTURE_BYTES));
    assert(!memcmp(memory+ATLAS+TEXTURE_BYTES+64,sets[BUTTON_ICONS_PS4],TEXTURE_BYTES));
    assert(invalidations==1 && invalidated==GUEST+ATLAS && invalidated_size==TEXTURE_BYTES);
    expect_mount("xbox");
    expect_pad(BUTTON_ICONS_XBOX);
    assert(pad_invalidations==1);

    use(BUTTON_ICONS_KEYBOARD);
    assert(!memcmp(memory+ATLAS,sets[BUTTON_ICONS_KEYBOARD],TEXTURE_BYTES));
    assert(invalidations==2);
    expect_mount("keyboard");
    expect_pad(BUTTON_ICONS_PS4); /* no keyboard pad: the DualShock 4 */
    assert(pad_invalidations==2);

    use(BUTTON_ICONS_PS4);
    assert(!memcmp(memory+ATLAS,sets[BUTTON_ICONS_PS4],TEXTURE_BYTES));
    expect_mount(NULL);
    expect_pad(BUTTON_ICONS_PS4);
    assert(pad_invalidations==2); /* the pad already showed the DualShock 4 */

    /* The game freed the atlas: nothing is written, only the mount follows (a later load is right). */
    memset(memory+ATLAS,0,TEXTURE_BYTES);
    use(BUTTON_ICONS_XBOX);
    assert(invalidations==3);
    for (size_t i=0;i<TEXTURE_BYTES;++i) assert(!memory[ATLAS+i]);
    expect_mount("xbox");

    /* Loaded again elsewhere (from the mounted bundle): found by a new search. */
    memcpy(memory+0x80000,sets[BUTTON_ICONS_XBOX],TEXTURE_BYTES);
    use(BUTTON_ICONS_KEYBOARD);
    assert(!memcmp(memory+0x80000,sets[BUTTON_ICONS_KEYBOARD],TEXTURE_BYTES));
    assert(invalidations==4 && invalidated==GUEST+0x80000);
    puts("button icons: ok");
    return 0;
}
