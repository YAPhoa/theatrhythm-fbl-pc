/* Button prompt icons and the Live Info pad. The game draws every prompt from one 512x256 BC7 atlas in
 * one bundle, and the pad of the Live Info panel and the tutorial from a 512x512 one in another.
 * scripts/button_icons.py builds Xbox and keyboard copies of the icon bundle in <out>/button_icons,
 * scripts/controller_pad.py an Xbox copy of the pad bundle in <out>/controller_pad, each with every
 * set's texture data as it lands in guest memory (<set>.texture). The pad has no keyboard drawing:
 * keyboard players keep the DualShock 4.
 *
 * A fixed set (settings.ini button_icons=xbox or keyboard) mounts its bundles over the game's at start.
 * "auto" follows the device the player last used (runtime_pad.c reports it): on a change the mounts
 * follow, so a later load of a bundle gets that set, and a worker thread finds each atlas already
 * in guest memory by its known bytes, writes the new set over it through the backing view and tells
 * the GPU the range changed, so the texture reloads on its next use. runtime_xbox_pad.c moves the
 * pad's highlights to match (runtime_button_icons_pad). */
#define _GNU_SOURCE
#include "runtime.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXTURE_BYTES (512*256)  /* the icon atlas: BC7, one byte per texel, no mipmaps */
#define PAD_BYTES (512*512)      /* the pad atlas */
#define TEXTURE_ALIGN 256        /* GCN texture base addresses are in 256-byte units */
#define MAX_COPIES 8
static const char *const prefixes[]={"/app0/","/archive/mount/point/"}; /* both paths the game may use */
static const char *const set_names[BUTTON_ICONS_SETS]={"ps4","xbox","keyboard"};

typedef struct {
    const char *what, *bundle;
    size_t bytes;
    int drawn[BUTTON_ICONS_SETS]; /* the set each set shows (the pad: keyboard shows ps4) */
    char folder[512];
    int enabled;                  /* auto: every texture loaded and told apart */
    unsigned char *textures[BUTTON_ICONS_SETS];
    size_t probe_offset;          /* 8 bytes that differ between every two drawn sets */
    uint64_t probes[BUTTON_ICONS_SETS];
    uintptr_t copies[MAX_COPIES]; size_t copy_count; /* atlases in guest memory (worker only) */
} Atlas;
enum { ICONS, PAD, ATLASES };
static Atlas atlases[ATLASES]={
    {.what="Button icons",.bundle="Media/StreamingAssets/aa/PS4/26f8caa7ece23b9c50dba3f9dd2af58f.bundle",
     .bytes=TEXTURE_BYTES,.drawn={BUTTON_ICONS_PS4,BUTTON_ICONS_XBOX,BUTTON_ICONS_KEYBOARD}},
    {.what="Xbox pad",.bundle="Media/StreamingAssets/aa/PS4/d9bc0bbf58d9e64cfb82c0837f3ab818.bundle",
     .bytes=PAD_BYTES,.drawn={BUTTON_ICONS_PS4,BUTTON_ICONS_XBOX,BUTTON_ICONS_PS4}},
};

static int automatic;
static int fixed_pad=BUTTON_ICONS_PS4; /* the pad set a fixed setting mounted */
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake=PTHREAD_COND_INITIALIZER;
static int wanted=BUTTON_ICONS_PS4, shown=BUTTON_ICONS_PS4; /* nothing mounted: the game's own icons */

static void bundle_file(const Atlas *a, int set, char *out, size_t size) { snprintf(out,size,"%s/%s.bundle",a->folder,set_names[set]); }
static int readable(const char *path) { FILE *f=fopen(path,"rb"); if (f) fclose(f); return f!=NULL; }

/* The game's bundle path serves `set`: its replacement, or the game's file for ps4. */
static int mount_set(const Atlas *a, int set) {
    char host[600], guest[128];
    set=a->drawn[set];
    bundle_file(a,set,host,sizeof(host));
    int failed=0;
    for (size_t i=0;i<sizeof(prefixes)/sizeof(*prefixes);++i) {
        snprintf(guest,sizeof(guest),"%s%s",prefixes[i],a->bundle);
        if (set==BUTTON_ICONS_PS4) runtime_file_unmount(guest);
        else failed|=runtime_file_mount(guest,host);
    }
    return failed;
}

static unsigned char *load_texture(const Atlas *a, int set) {
    char path[600];
    snprintf(path,sizeof(path),"%s/%s.texture",a->folder,set_names[set]);
    FILE *f=fopen(path,"rb");
    unsigned char *data=f ? malloc(a->bytes+1) : NULL;
    size_t n=data ? fread(data,1,a->bytes+1,f) : 0;
    if (f) fclose(f);
    if (n==a->bytes) return data;
    free(data);
    printf("%s: %s %s (python run.py prepare); PS4 %s\n",a->what,path,f ? "is not an atlas texture" : "missing",
           a==&atlases[PAD] ? "pad" : "icons");
    return NULL;
}

/* The drawn set an atlas-sized block holds, or -1. */
static int set_of(const Atlas *a, const unsigned char *p) {
    uint64_t v; memcpy(&v,p+a->probe_offset,8);
    for (int set=0;set<BUTTON_ICONS_SETS;++set)
        if (a->drawn[set]==set && v==a->probes[set] && !memcmp(p,a->textures[set],a->bytes)) return set;
    return -1;
}

typedef struct { const Atlas *atlas; uintptr_t found[MAX_COPIES]; size_t count; } Scan;
static void scan_range(uintptr_t address, const unsigned char *bytes, uint64_t size, void *context) {
    Scan *s=context;
    const Atlas *a=s->atlas;
    const uintptr_t end=address+size;
    for (uintptr_t at=(address+TEXTURE_ALIGN-1)&~(uintptr_t)(TEXTURE_ALIGN-1);
         at+a->bytes<=end && s->count<MAX_COPIES; at+=TEXTURE_ALIGN) {
        const unsigned char *p=bytes+(at-address);
        uint64_t v; memcpy(&v,p+a->probe_offset,8);
        int candidate=0;
        for (int set=0;set<BUTTON_ICONS_SETS;++set) candidate|=a->drawn[set]==set && v==a->probes[set];
        if (candidate && set_of(a,p)>=0) s->found[s->count++]=at;
    }
}

/* Writes `set` over every copy of the atlas in guest memory. Known copies are checked again first
 * (the game may have freed one); memory is searched only when none is left. */
static void show(Atlas *a, int set) {
    static unsigned char current[PAD_BYTES];
    set=a->drawn[set];
    size_t kept=0;
    for (size_t i=0;i<a->copy_count;++i) {
        runtime_memory_read_backing(a->copies[i],current,a->bytes);
        if (set_of(a,current)>=0) a->copies[kept++]=a->copies[i];
    }
    a->copy_count=kept;
    if (!a->copy_count) {
        Scan s={a,{0},0};
        runtime_memory_visit_mapped(scan_range,&s);
        memcpy(a->copies,s.found,s.count*sizeof(*a->copies));
        a->copy_count=s.count;
    }
    size_t written=0;
    for (size_t i=0;i<a->copy_count;++i) {
        runtime_memory_read_backing(a->copies[i],current,a->bytes);
        const int had=set_of(a,current);
        if (had<0 || had==set) continue;
        if (runtime_memory_write_backing(a->copies[i],a->textures[set],a->bytes)) {
            runtime_memory_invalidate(a->copies[i],a->bytes);
            ++written;
        }
    }
    printf("%s: %s (%zu of %zu loaded atlases rewritten)\n",a->what,set_names[set],written,a->copy_count);
}

static void *worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&lock);
    for (;;) {
        while (wanted==shown) pthread_cond_wait(&wake,&lock);
        const int set=wanted, before=shown;
        pthread_mutex_unlock(&lock);
        for (int i=0;i<ATLASES;++i) {
            Atlas *a=&atlases[i];
            if (!a->enabled || a->drawn[set]==a->drawn[before]) continue;
            if (mount_set(a,set)) printf("%s: cannot mount the %s bundle\n",a->what,set_names[set]);
            show(a,set);
        }
        pthread_mutex_lock(&lock);
        __atomic_store_n(&shown,set,__ATOMIC_RELEASE);
        pthread_cond_broadcast(&wake);
    }
    return NULL;
}

/* Every drawn set's texture of `a`, and 8 bytes that tell them apart; 0 when that fails. */
static int load_atlas(Atlas *a) {
    for (int set=0;set<BUTTON_ICONS_SETS;++set) {
        if (a->drawn[set]!=set) continue;
        char host[600];
        bundle_file(a,set,host,sizeof(host));
        if (set!=BUTTON_ICONS_PS4 && !readable(host)) {
            printf("%s: %s missing (python run.py prepare); PS4 %s\n",a->what,host,a==&atlases[PAD] ? "pad" : "icons");
            return 0;
        }
        if (!(a->textures[set]=load_texture(a,set))) return 0;
    }
    for (a->probe_offset=0; a->probe_offset<a->bytes; a->probe_offset+=8) {
        int distinct=1;
        for (int set=0;set<BUTTON_ICONS_SETS;++set) if (a->drawn[set]==set) memcpy(&a->probes[set],a->textures[set]+a->probe_offset,8);
        for (int i=0;i<BUTTON_ICONS_SETS;++i)
            for (int j=i+1;j<BUTTON_ICONS_SETS;++j)
                if (a->drawn[i]==i && a->drawn[j]==j && a->probes[i]==a->probes[j]) distinct=0;
        if (distinct) return 1;
    }
    printf("%s: the sets are identical; PS4 %s\n",a->what,a==&atlases[PAD] ? "pad" : "icons");
    return 0;
}

/* A fixed set: mount `a`'s bundle for it. 1 when mounted. */
static int mount_fixed(const Atlas *a, int set) {
    char host[600];
    if (a->drawn[set]==BUTTON_ICONS_PS4) return 0;
    bundle_file(a,a->drawn[set],host,sizeof(host));
    if (!readable(host)) printf("%s: %s missing (python run.py prepare); PS4 %s\n",a->what,host,a==&atlases[PAD] ? "pad" : "icons");
    else if (mount_set(a,set)) printf("%s: cannot mount %s; PS4 %s\n",a->what,host,a==&atlases[PAD] ? "pad" : "icons");
    else { printf("%s: %s\n",a->what,set_names[set]); return 1; }
    return 0;
}

void runtime_button_icons_configure(const char *icons, const char *pad, const char *mode) {
    snprintf(atlases[ICONS].folder,sizeof(atlases[ICONS].folder),"%s",icons);
    snprintf(atlases[PAD].folder,sizeof(atlases[PAD].folder),"%s",pad);
    int fixed=-1;
    for (int set=0;set<BUTTON_ICONS_SETS;++set) if (!strcmp(mode,set_names[set])) fixed=set;
    if (fixed==BUTTON_ICONS_PS4) return;
    if (fixed>0) {
        mount_fixed(&atlases[ICONS],fixed);
        if (mount_fixed(&atlases[PAD],fixed)) fixed_pad=atlases[PAD].drawn[fixed];
        return;
    }
    if (strcmp(mode,"auto")) return;
    int any=0;
    for (int i=0;i<ATLASES;++i) any|=atlases[i].enabled=load_atlas(&atlases[i]);
    if (!any) return;
    pthread_t thread;
    if (pthread_create(&thread,NULL,worker,NULL)) {
        puts("Button icons: no worker thread; PS4 icons and pad");
        for (int i=0;i<ATLASES;++i) atlases[i].enabled=0;
        return;
    }
    pthread_detach(thread);
    __atomic_store_n(&automatic,1,__ATOMIC_RELEASE);
    puts("Button icons: auto (follow the device last used)");
}

/* The player used a device whose icons are `set`; called for every sample with input. */
void runtime_button_icons_input(int set) {
    if (!__atomic_load_n(&automatic,__ATOMIC_ACQUIRE) || set<0 || set>=BUTTON_ICONS_SETS) return;
    if (__atomic_load_n(&wanted,__ATOMIC_RELAXED)==set) return;
    pthread_mutex_lock(&lock);
    if (wanted!=set) { __atomic_store_n(&wanted,set,__ATOMIC_RELAXED); pthread_cond_signal(&wake); }
    pthread_mutex_unlock(&lock);
}

/* The pad set the game's pad atlas shows now (BUTTON_ICONS_PS4 or _XBOX). */
int runtime_button_icons_pad(void) {
    if (!__atomic_load_n(&automatic,__ATOMIC_ACQUIRE)) return fixed_pad;
    if (!atlases[PAD].enabled) return BUTTON_ICONS_PS4;
    return atlases[PAD].drawn[__atomic_load_n(&shown,__ATOMIC_ACQUIRE)];
}
