/* libSceSaveData on host directories:
 *   <user>/savedata/<user id>/<title id>/<dir name>/        files the game writes
 *   <user>/savedata/<user id>/<title id>/<dir name>.sce_sys/ param.bin, icon0.png
 *   <user>/savedata/<user id>/<title id>/<dir name>.sce_backup/  backup: data/ and sce_sys/
 *   <user>/savedata/<user id>/<title id>.memory/memory.dat  SaveDataMemory (slot 0; slot n:
 *                                                 <title id>.memory<n>/), with param.bin, icon0.png
 * A mounted directory appears to the guest as /savedata0../savedata15.
 * Metadata lives beside the directory so the guest's own files are untouched. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef _WIN32
#include <ftw.h>
#endif

#define ERR_PARAMETER ((int32_t)0x809F0000)
#define ERR_NOT_INITIALIZED ((int32_t)0x809F0001)
#define ERR_BUSY ((int32_t)0x809F0003)
#define ERR_NOT_MOUNTED ((int32_t)0x809F0004)
#define ERR_EXISTS ((int32_t)0x809F0007)
#define ERR_NOT_FOUND ((int32_t)0x809F0008)
#define ERR_INTERNAL ((int32_t)0x809F000B)
#define ERR_MOUNT_FULL ((int32_t)0x809F000C)
#define ERR_BAD_MOUNTED ((int32_t)0x809F000D)
#define ERR_INVALID_USER ((int32_t)0x809F0011)
#define ERR_MEMORY_NOT_READY ((int32_t)0x809F0012)
#define BLOCK_SIZE 32768
#define DEFAULT_BLOCKS 32768
#define MODE_RDONLY 1
#define MODE_CREATE 4
#define MODE_COPY_ICON 16
#define MODE_CREATE2 32
#define SLOTS 16

typedef struct { char data[10]; char pad[6]; } TitleId;
typedef struct { char data[32]; } DirName;
typedef struct { char data[16]; } MountPoint;
typedef struct {
    char title[128], subtitle[128], detail[1024];
    uint32_t user_param; int32_t pad;
    int64_t mtime;
    uint8_t reserved[32];
} Param;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint;
                 uint64_t blocks; uint32_t mode; uint8_t reserved[32]; } Mount1;
typedef struct { int32_t user; int32_t pad; const DirName *dir; uint64_t blocks; uint32_t mode;
                 uint8_t reserved[32]; int32_t pad2; } Mount2;
typedef struct { MountPoint point; uint64_t required_blocks; uint32_t unused, status; uint8_t reserved[28]; int32_t pad; } MountResult;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t unused;
                 uint8_t reserved[32]; int32_t pad2; } Delete;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t key, order;
                 uint8_t reserved[32]; } SearchCond;
typedef struct { uint64_t blocks, free_blocks; uint8_t reserved[32]; } SearchInfo;
typedef struct { uint32_t hits; int32_t pad; DirName *names; uint32_t names_capacity, set_count;
                 Param *params; SearchInfo *infos; uint8_t reserved[12]; int32_t pad2; } SearchResult;
typedef struct { const void *buffer; uint64_t buffer_size, data_size; uint8_t reserved[32]; } Icon;
_Static_assert(sizeof(Param)==1328,"OrbisSaveDataParam layout");
_Static_assert(sizeof(Mount1)==80 && sizeof(Mount2)==64,"OrbisSaveDataMount layouts");
_Static_assert(sizeof(MountResult)==64,"OrbisSaveDataMountResult layout");
_Static_assert(sizeof(SearchCond)==64 && sizeof(SearchResult)==56,"dir name search layouts");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized;
static char title_id[16]="UNKNOWN";
static struct { int used; int32_t user; uint64_t blocks; char host[700], meta[700]; } slots[SLOTS];
static struct { char path[720], dir[700]; uint64_t size; } memories[SLOTS]; /* SaveDataMemory slots */
static size_t mounts_done, memory_writes, backups_done;

void runtime_savedata_configure(const char *title) {
    if (title && *title) snprintf(title_id,sizeof(title_id),"%s",title);
}

static int make_dirs(const char *path) {
    char buffer[700];
    snprintf(buffer,sizeof(buffer),"%s",path);
    for (char *p=buffer+1;*p;++p) if (*p=='/') { *p=0; mkdir(buffer,0755); *p='/'; }
    return mkdir(buffer,0755) && errno!=EEXIST ? -1 : 0;
}
static void root(int32_t user, const char *title, char *out, size_t size) {
    snprintf(out,size,"%s/savedata/%d/%s",runtime_file_user_dir(),user,title && *title ? title : title_id);
}
static int valid_name(const char *name, size_t max) {
    size_t n=strnlen(name,max);
    if (!n || n==max) return 0;
    for (size_t i=0;i<n;++i) if (name[i]=='/' || name[i]=='\\' || (name[i]=='.' && (i==0 || name[i-1]=='.'))) return 0;
    return 1;
}
static void write_param(const char *meta, const Param *p) {
    char path[700]; snprintf(path,sizeof(path),"%s/param.bin",meta);
    FILE *f=fopen(path,"wb");
    if (f) { Param copy=*p; copy.mtime=time(NULL); fwrite(&copy,sizeof(copy),1,f); fclose(f); }
}
static int read_param(const char *meta, Param *p) {
    char path[700]; snprintf(path,sizeof(path),"%s/param.bin",meta);
    memset(p,0,sizeof(*p));
    FILE *f=fopen(path,"rb");
    if (!f) return -1;
    size_t n=fread(p,sizeof(*p),1,f); fclose(f);
    struct stat st;
    if (!stat(path,&st)) p->mtime=st.st_mtime;
    return n==1 ? 0 : -1;
}
#ifdef _WIN32
static void remove_tree(const char *path) { compat_remove_tree(path); }
#else
static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)flag; (void)ftw; return remove(path);
}
static void remove_tree(const char *path) { nftw(path,remove_entry,16,FTW_DEPTH|FTW_PHYS); }
#endif

/* A save's param (title, subtitle, date) for the save dialog; 0 when it exists. */
int runtime_savedata_param(int32_t user, const char *dir, void *param) {
    if (!valid_name(dir,32)) return -1;
    char base[600], meta[680];
    root(user,NULL,base,sizeof(base));
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,dir);
    return read_param(meta,(Param *)param);
}
/* Host path of a save's icon0.png (the location picture in the save dialog); 0 when named. */
int runtime_savedata_icon(int32_t user, const char *dir, char *out, size_t size) {
    if (!valid_name(dir,32)) return -1;
    char base[600];
    root(user,NULL,base,sizeof(base));
    snprintf(out,size,"%s/%s.sce_sys/icon0.png",base,dir);
    return 0;
}
/* Newest modification time (seconds) of a save: its files and its param.bin; 0 when absent. */
static int64_t newest(const char *path, int depth) {
    struct stat st;
#ifndef _WIN32
    if (lstat(path,&st)) return 0;
    if (S_ISLNK(st.st_mode)) return 0; /* do not follow symlinks outside save */
#else
    if (stat(path,&st)) return 0;
#endif
    if (!S_ISDIR(st.st_mode)) return (int64_t)(st.st_mtime>0 ? st.st_mtime : 0);
    if (depth>4) return 0;
    int64_t t=0;
    DIR *d=opendir(path);
    for (struct dirent *e; d && (e=readdir(d));) {
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        char child[1024];
        if (snprintf(child,sizeof(child),"%s/%s",path,e->d_name)>=(int)sizeof(child)) continue;
        int64_t c=newest(child,depth+1);
        if (c>t) t=c;
    }
    if (d) closedir(d);
    return t;
}
/* When a save was last written (the load list's focus, the menu's last autosave); 0 when absent. */
int64_t runtime_savedata_mtime(int32_t user, const char *dir) {
    if (!valid_name(dir,32)) return 0;
    char base[600], path[700];
    root(user,NULL,base,sizeof(base));
    snprintf(path,sizeof(path),"%s/%s",base,dir);
    struct stat st;
    if (stat(path,&st) || !S_ISDIR(st.st_mode)) return 0;
    int64_t t=newest(path,0);
    snprintf(path,sizeof(path),"%s/%s.sce_sys/param.bin",base,dir);
    if (!stat(path,&st) && (int64_t)st.st_mtime>t) t=(int64_t)st.st_mtime;
    return t>0 ? t : 0;
}
static ABI int32_t save_initialize(const void *param) { (void)param; initialized=1; return 0; }
static ABI int32_t save_terminate(void) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    initialized=0; return 0;
}
static int32_t mount(int32_t user, const char *title, const DirName *dir, uint32_t mode, uint64_t blocks, MountResult *result) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user<0) return ERR_INVALID_USER;
    if (!dir || !result || !valid_name(dir->data,sizeof(dir->data))) return ERR_PARAMETER;
    char base[600], host[640], meta[680];
    root(user,title,base,sizeof(base));
    snprintf(host,sizeof(host),"%s/%s",base,dir->data);
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,dir->data);
    struct stat st;
    int exists=!stat(host,&st) && S_ISDIR(st.st_mode);
    if ((mode & MODE_CREATE) && exists) return ERR_EXISTS;
    if (!(mode & (MODE_CREATE|MODE_CREATE2)) && !exists) return ERR_NOT_FOUND;
    pthread_mutex_lock(&lock);
    int slot=-1;
    for (int i=0;i<SLOTS;++i) {
        if (slots[i].used && !strcmp(slots[i].host,host)) { pthread_mutex_unlock(&lock); return ERR_BAD_MOUNTED; }
        if (!slots[i].used && slot<0) slot=i;
    }
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_MOUNT_FULL; }
    if (!exists && (make_dirs(host) || make_dirs(meta))) { pthread_mutex_unlock(&lock); return ERR_INTERNAL; }
    if (!exists) { Param empty={0}; write_param(meta,&empty); }
    slots[slot].used=1;
    slots[slot].blocks=blocks ? blocks : DEFAULT_BLOCKS;
    slots[slot].user=user;
    snprintf(slots[slot].host,sizeof(slots[slot].host),"%s",host);
    snprintf(slots[slot].meta,sizeof(slots[slot].meta),"%s",meta);
    memset(result,0,sizeof(*result));
    char point[32];
    snprintf(point,sizeof(point),"/savedata%d",slot&15);
    memcpy(result->point.data,point,strlen(point)+1);
    result->status=exists ? 0 : 1; /* CREATED */
    runtime_file_mount(result->point.data,host);
    ++mounts_done;
    pthread_mutex_unlock(&lock);
    printf("Runtime: save data '%s' mounted at %s (%s%s)\n",dir->data,result->point.data,
           exists ? "existing" : "created",(mode & MODE_RDONLY) ? ", read-only" : "");
    return 0;
}
static ABI int32_t save_mount(const Mount1 *m, MountResult *result) {
    if (!m) return ERR_PARAMETER;
    return mount(m->user,m->title ? m->title->data : NULL,m->dir,m->mode,m->blocks,result);
}
static ABI int32_t save_mount2(const Mount2 *m, MountResult *result) {
    if (!m) return ERR_PARAMETER;
    return mount(m->user,NULL,m->dir,m->mode,m->blocks,result);
}
static int slot_of(const MountPoint *point) {
    if (!point || strncmp(point->data,"/savedata",9)) return -1;
    int slot=atoi(point->data+9);
    return slot>=0 && slot<SLOTS && slots[slot].used ? slot : -1;
}
static ABI int32_t save_umount(const MountPoint *point) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    if (slot>=0) { runtime_file_unmount(point->data); slots[slot].used=0; }
    pthread_mutex_unlock(&lock);
    return slot>=0 ? 0 : ERR_NOT_FOUND;
}
static ABI int32_t save_set_param(const MountPoint *point, uint32_t type, const void *buffer, uint64_t size) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_NOT_MOUNTED; }
    Param p; read_param(slots[slot].meta,&p);
    switch (type) {
    case 0: if (size<sizeof(Param)) goto bad; memcpy(&p,buffer,sizeof(p)); break;     /* ALL */
    case 1: snprintf(p.title,sizeof(p.title),"%.*s",(int)size,(const char *)buffer); break;
    case 2: snprintf(p.subtitle,sizeof(p.subtitle),"%.*s",(int)size,(const char *)buffer); break;
    case 3: snprintf(p.detail,sizeof(p.detail),"%.*s",(int)size,(const char *)buffer); break;
    case 4: if (size<4) goto bad; memcpy(&p.user_param,buffer,4); break;
    default: goto bad;
    }
    write_param(slots[slot].meta,&p);
    pthread_mutex_unlock(&lock);
    return 0;
bad:
    pthread_mutex_unlock(&lock);
    return ERR_PARAMETER;
}
static ABI int32_t save_icon(const MountPoint *point, const Icon *icon) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!icon || !icon->buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    int32_t r=ERR_NOT_MOUNTED;
    if (slot>=0) {
        char path[700]; snprintf(path,sizeof(path),"%s/icon0.png",slots[slot].meta);
        FILE *f=fopen(path,"wb");
        r=f && fwrite(icon->buffer,1,icon->data_size,f)==icon->data_size ? 0 : ERR_INTERNAL;
        if (f) fclose(f);
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t save_delete(const Delete *d) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!d || !d->dir || !valid_name(d->dir->data,sizeof(d->dir->data))) return ERR_PARAMETER;
    char base[600], host[640], meta[680];
    root(d->user,d->title ? d->title->data : NULL,base,sizeof(base));
    snprintf(host,sizeof(host),"%s/%s",base,d->dir->data);
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,d->dir->data);
    struct stat st;
    if (stat(host,&st)) return ERR_NOT_FOUND;
    remove_tree(host);
    remove_tree(meta);
    printf("Runtime: save data '%s' deleted\n",d->dir->data);
    return 0;
}
/* SQL-LIKE pattern: % any run, _ one character. */
static int like(const char *s, const char *p) {
    if (!*p) return !*s;
    if (*p=='%') { for (;;++s) { if (like(s,p+1)) return 1; if (!*s) return 0; } }
    return *s && (*p=='_' || *p==*s) && like(s+1,p+1);
}
typedef struct { char name[32]; Param param; } Entry;
static uint32_t sort_key, sort_order;
static int compare(const void *a, const void *b) {
    const Entry *x=a, *y=b;
    int c = sort_key==1 ? (x->param.user_param>y->param.user_param)-(x->param.user_param<y->param.user_param)
          : sort_key==3 ? (x->param.mtime>y->param.mtime)-(x->param.mtime<y->param.mtime)
          : strcmp(x->name,y->name);
    return sort_order ? -c : c;
}
static ABI int32_t save_search(const SearchCond *cond, SearchResult *result) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!cond || !result) return ERR_PARAMETER;
    char base[600];
    root(cond->user,cond->title ? cond->title->data : NULL,base,sizeof(base));
    Entry *entries=NULL; size_t count=0, capacity=0;
    DIR *d=opendir(base);
    for (struct dirent *e; d && (e=readdir(d));) {
        size_t n=strlen(e->d_name);
        if (e->d_name[0]=='.' || n>=32 || (n>8 && !strcmp(e->d_name+n-8,".sce_sys")) ||
            (n>11 && !strcmp(e->d_name+n-11,".sce_backup"))) continue;
        if (cond->dir && cond->dir->data[0] && !like(e->d_name,cond->dir->data)) continue;
        if (count==capacity) { capacity=capacity ? capacity*2 : 16; entries=realloc(entries,capacity*sizeof(*entries)); }
        snprintf(entries[count].name,32,"%s",e->d_name);
        char meta[680]; snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,e->d_name);
        read_param(meta,&entries[count].param);
        ++count;
    }
    if (d) closedir(d);
    sort_key=cond->key; sort_order=cond->order;
    if (count) qsort(entries,count,sizeof(*entries),compare);
    result->hits=(uint32_t)count;
    uint32_t set=count<result->names_capacity ? (uint32_t)count : result->names_capacity;
    for (uint32_t i=0;i<set;++i) {
        if (result->names) memcpy(result->names[i].data,entries[i].name,32);
        if (result->params) result->params[i]=entries[i].param;
        if (result->infos) { memset(&result->infos[i],0,sizeof(SearchInfo)); result->infos[i].blocks=32768; result->infos[i].free_blocks=16384; }
    }
    result->set_count=set;
    free(entries);
    return 0;
}
/* SaveDataMemory: one fixed-size blob per user/title and slot (shadPS4 save_memory.cpp). Setup
 * creates or keeps it (an existing file only grows) and reports the size it had, 0 when new. */
static int32_t memory_setup_slot(int32_t user, uint32_t slot, uint64_t size, const Param *param,
                                 const Icon *icon, uint64_t *existed) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!size || slot>=SLOTS) return ERR_PARAMETER;
    char base[600], dir[640];
    root(user,NULL,base,sizeof(base));
    if (slot) snprintf(dir,sizeof(dir),"%s.memory%u",base,slot);
    else snprintf(dir,sizeof(dir),"%s.memory",base);
    if (make_dirs(dir)) return ERR_INTERNAL;
    pthread_mutex_lock(&lock);
    snprintf(memories[slot].dir,sizeof(memories[slot].dir),"%s",dir);
    snprintf(memories[slot].path,sizeof(memories[slot].path),"%s/memory.dat",dir);
    FILE *f=fopen(memories[slot].path,"r+b");
    int created=!f;
    if (!f) f=fopen(memories[slot].path,"w+b");
    int32_t r=ERR_INTERNAL;
    uint64_t had=0;
    if (f) {
        fseek(f,0,SEEK_END);
        had=(uint64_t)ftell(f);
        if (had<size && ftruncate(fileno(f),(off_t)size)) goto done;
        memories[slot].size=size; r=0;
    done:
        fclose(f);
    }
    if (!r && created && param) write_param(dir,param);
    if (!r && created && icon && icon->buffer) {
        char path[720]; snprintf(path,sizeof(path),"%s/icon0.png",dir);
        FILE *i=fopen(path,"wb");
        if (i) { fwrite(icon->buffer,1,icon->buffer_size,i); fclose(i); }
    }
    pthread_mutex_unlock(&lock);
    if (existed) *existed=created ? 0 : had;
    if (!r) printf("Runtime: save data memory slot %u ready (%llu bytes)\n",slot,(unsigned long long)size);
    return r;
}
static ABI int32_t memory_setup(int32_t user, uint64_t size, const Param *param) {
    return memory_setup_slot(user,0,size,param,NULL,NULL);
}
static int32_t memory_io(uint32_t slot, void *buffer, uint64_t size, int64_t offset, int write) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!buffer || offset<0 || slot>=SLOTS) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    if (!memories[slot].size) { pthread_mutex_unlock(&lock); return ERR_MEMORY_NOT_READY; }
    if ((uint64_t)offset+size>memories[slot].size) { pthread_mutex_unlock(&lock); return ERR_PARAMETER; }
    FILE *f=fopen(memories[slot].path,"r+b");
    int32_t r=ERR_INTERNAL;
    if (f && !fseek(f,offset,SEEK_SET) &&
        (write ? fwrite(buffer,1,size,f) : fread(buffer,1,size,f))==size) r=0;
    if (f) fclose(f);
    if (!r && write) ++memory_writes;
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t memory_get(int32_t user, void *buffer, uint64_t size, int64_t offset) { (void)user; return memory_io(0,buffer,size,offset,0); }
static ABI int32_t memory_set(int32_t user, void *buffer, uint64_t size, int64_t offset) { (void)user; return memory_io(0,buffer,size,offset,1); }
typedef struct { void *buffer; uint64_t size; int64_t offset; uint8_t reserved[40]; } MemoryData;
typedef struct { uint32_t option; int32_t user; uint64_t size, icon_size; const Param *param; const Icon *icon;
                 uint32_t slot; uint8_t reserved[20]; } MemorySetup2;
typedef struct { uint64_t existed; uint8_t reserved[16]; } MemorySetupResult;
typedef struct { int32_t user; int32_t pad; MemoryData *data; Param *param; Icon *icon; uint32_t slot;
                 uint8_t reserved[28]; } MemoryGet2;
typedef struct { int32_t user; int32_t pad; const MemoryData *data; const Param *param; const Icon *icon;
                 uint32_t count, slot; uint8_t reserved[32]; } MemorySet2;
typedef struct { int32_t user; uint32_t slot, option; uint8_t reserved[28]; } MemorySync;
_Static_assert(sizeof(MemorySetup2)==64 && sizeof(MemoryGet2)==64 && sizeof(MemorySet2)==72,"SaveDataMemory v2 layouts");
/* An icon file into an Icon: up to its buffer, data_size the file size (shadPS4 LoadIcon). */
static int32_t load_icon(const char *path, Icon *icon) {
    FILE *f=fopen(path,"rb");
    if (!f) return ERR_NOT_FOUND;
    fseek(f,0,SEEK_END);
    uint64_t size=(uint64_t)ftell(f);
    fseek(f,0,SEEK_SET);
    uint64_t n=size<icon->buffer_size ? size : icon->buffer_size;
    int ok=fread((void *)icon->buffer,1,n,f)==n;
    fclose(f);
    icon->data_size=size;
    return ok ? 0 : ERR_INTERNAL;
}
static ABI int32_t memory_setup2(const MemorySetup2 *setup, MemorySetupResult *result) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!setup) return ERR_PARAMETER;
    uint64_t existed=0;
    int32_t r=memory_setup_slot(setup->user,setup->slot,setup->size,setup->param,setup->icon,&existed);
    if (!r && result) result->existed=existed;
    return r;
}
static ABI int32_t memory_get2(MemoryGet2 *get) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!get) return ERR_PARAMETER;
    if (get->slot>=SLOTS || !memories[get->slot].size) return ERR_MEMORY_NOT_READY;
    if (get->data) { int32_t r=memory_io(get->slot,get->data->buffer,get->data->size,get->data->offset,0); if (r) return r; }
    if (get->param) read_param(memories[get->slot].dir,get->param);
    if (get->icon && get->icon->buffer) {
        char path[720]; snprintf(path,sizeof(path),"%s/icon0.png",memories[get->slot].dir);
        if (load_icon(path,get->icon)) get->icon->data_size=0;
    }
    return 0;
}
static ABI int32_t memory_set2(const MemorySet2 *set) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!set) return ERR_PARAMETER;
    if (set->slot>=SLOTS || !memories[set->slot].size) return ERR_MEMORY_NOT_READY;
    uint32_t count=set->count>1 ? set->count : 1;
    for (uint32_t i=0; set->data && i<count; ++i) {
        int32_t r=memory_io(set->slot,set->data[i].buffer,set->data[i].size,set->data[i].offset,1);
        if (r) return r;
    }
    if (set->param) write_param(memories[set->slot].dir,set->param);
    if (set->icon && set->icon->buffer) {
        char path[720]; snprintf(path,sizeof(path),"%s/icon0.png",memories[set->slot].dir);
        FILE *f=fopen(path,"wb");
        if (f) { fwrite(set->icon->buffer,1,set->icon->buffer_size,f); fclose(f); }
    }
    return 0;
}

/* ---- Backups (shadPS4 save_backup.cpp), done at once on the calling thread ----
 * A backup copies the save's files to <dir>.sce_backup/data and its param.bin/icon0.png to
 * <dir>.sce_backup/sce_sys; each completed one queues an event for sceSaveDataGetEventResult
 * (1 umount backup, 2 backup, 3 memory sync) and sets the progress to 1. A mounted save is BUSY. */
typedef struct { uint32_t type; int32_t error, user; uint8_t pad[4]; TitleId title; DirName dir; uint8_t reserved[40]; } Event;
_Static_assert(sizeof(Event)==104,"OrbisSaveDataEvent layout");
static Event events[16];
static unsigned event_count;
static float progress;
static void push_event(uint32_t type, int32_t user, const char *title, const char *dir) {
    pthread_mutex_lock(&lock);
    if (event_count<16) {
        Event *e=&events[event_count++];
        memset(e,0,sizeof(*e));
        e->type=type; e->user=user;
        snprintf(e->title.data,sizeof(e->title.data),"%s",title && *title ? title : title_id);
        snprintf(e->dir.data,sizeof(e->dir.data),"%s",dir ? dir : "");
    }
    progress=1.0f;
    pthread_mutex_unlock(&lock);
}
static int copy_file(const char *from, const char *to) {
    FILE *in=fopen(from,"rb");
    if (!in) return -1;
    FILE *out=fopen(to,"wb");
    if (!out) { fclose(in); return -1; }
    char buffer[65536];
    size_t n;
    int r=0;
    while ((n=fread(buffer,1,sizeof(buffer),in))) if (fwrite(buffer,1,n,out)!=n) { r=-1; break; }
    fclose(in); fclose(out);
    return r;
}
static int copy_tree(const char *from, const char *to, int depth) {
    struct stat st;
    if (stat(from,&st)) return -1;
    if (!S_ISDIR(st.st_mode)) return copy_file(from,to);
    if (depth>16 || make_dirs(to)) return -1;
    DIR *d=opendir(from);
    int r=d ? 0 : -1;
    for (struct dirent *e; d && (e=readdir(d));) {
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        char a[1024], b[1024];
        if (snprintf(a,sizeof(a),"%s/%s",from,e->d_name)>=(int)sizeof(a) ||
            snprintf(b,sizeof(b),"%s/%s",to,e->d_name)>=(int)sizeof(b) || copy_tree(a,b,depth+1)) r=-1;
    }
    if (d) closedir(d);
    return r;
}
/* Host paths of a save: its files, its metadata and its backup; -1 for a bad name. */
static int save_paths(int32_t user, const char *title, const DirName *dir, char host[640], char meta[680], char backup[680]) {
    if (!dir || !valid_name(dir->data,sizeof(dir->data))) return -1;
    char base[600];
    root(user,title,base,sizeof(base));
    snprintf(host,640,"%s/%s",base,dir->data);
    snprintf(meta,680,"%s/%s.sce_sys",base,dir->data);
    snprintf(backup,680,"%s/%s.sce_backup",base,dir->data);
    return 0;
}
static int is_mounted(const char *host) {
    pthread_mutex_lock(&lock);
    int found=0;
    for (int i=0;i<SLOTS;++i) found|=slots[i].used && !strcmp(slots[i].host,host);
    pthread_mutex_unlock(&lock);
    return found;
}
static int backups_running; /* make_backup copying (sceSaveDataUmountWithBackup frees the slot first) */
static int32_t make_backup(const char *host, const char *meta, const char *backup) {
    char data[720], sys[720];
    snprintf(data,sizeof(data),"%s/data",backup);
    snprintf(sys,sizeof(sys),"%s/sce_sys",backup);
    __atomic_add_fetch(&backups_running,1,__ATOMIC_ACQ_REL);
    remove_tree(backup);
    int32_t r=0;
    if (copy_tree(host,data,0) || copy_tree(meta,sys,0)) { remove_tree(backup); r=ERR_INTERNAL; }
    else ++backups_done;
    __atomic_sub_fetch(&backups_running,1,__ATOMIC_ACQ_REL);
    return r;
}
/* 1 while the game has a save mounted, a backup copying or a SaveDataMemory write going on (the
 * lock waits that out): F4's Yes ends the game only after it (runtime_pause.c). */
int runtime_savedata_busy(void) {
    pthread_mutex_lock(&lock);
    int busy=__atomic_load_n(&backups_running,__ATOMIC_ACQUIRE)>0;
    for (int i=0;i<SLOTS && !busy;++i) busy=slots[i].used;
    pthread_mutex_unlock(&lock);
    return busy;
}
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint;
                 uint8_t reserved[32]; } BackupRequest;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; Param *param; Icon *icon;
                 uint8_t reserved[32]; } CheckBackup;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint;
                 uint32_t unused; uint8_t reserved[32]; int32_t pad2; } RestoreBackup;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint;
                 uint8_t reserved[32]; } TransferringMount;
/* sceSaveDataBackup(request): copy the (unmounted) save now. */
static ABI int32_t save_backup(const BackupRequest *b) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    char host[640], meta[680], backup[680];
    const char *title=b && b->title ? b->title->data : NULL;
    if (!b || save_paths(b->user,title,b->dir,host,meta,backup)) return ERR_PARAMETER;
    if (is_mounted(host)) return ERR_BUSY;
    struct stat st;
    if (stat(host,&st)) return ERR_NOT_FOUND;
    int32_t r=make_backup(host,meta,backup);
    if (!r) push_event(2,b->user,title,b->dir->data);
    return r;
}
/* sceSaveDataCheckBackupData(check): NOT_FOUND without a backup; its param and icon otherwise. */
static ABI int32_t save_check_backup(const CheckBackup *c) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    char host[640], meta[680], backup[680], path[720];
    if (!c || save_paths(c->user,c->title ? c->title->data : NULL,c->dir,host,meta,backup)) return ERR_PARAMETER;
    if (is_mounted(host)) return ERR_BUSY;
    struct stat st;
    if (stat(backup,&st)) return ERR_NOT_FOUND;
    snprintf(path,sizeof(path),"%s/sce_sys",backup);
    if (c->param && read_param(path,c->param)) return ERR_INTERNAL;
    snprintf(path,sizeof(path),"%s/sce_sys/icon0.png",backup);
    if (c->icon && c->icon->buffer && !stat(path,&st) && load_icon(path,c->icon)) return ERR_INTERNAL;
    return 0;
}
/* sceSaveDataRestoreBackupData(restore): the backup replaces the save's files and metadata. */
static ABI int32_t save_restore_backup(const RestoreBackup *r) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    char host[640], meta[680], backup[680], data[720], sys[720];
    if (!r || save_paths(r->user,r->title ? r->title->data : NULL,r->dir,host,meta,backup)) return ERR_PARAMETER;
    if (is_mounted(host)) return ERR_BUSY;
    struct stat st;
    if (stat(backup,&st)) return ERR_NOT_FOUND;
    snprintf(data,sizeof(data),"%s/data",backup);
    snprintf(sys,sizeof(sys),"%s/sce_sys",backup);
    remove_tree(host); remove_tree(meta);
    if (copy_tree(data,host,0) || copy_tree(sys,meta,0)) return ERR_INTERNAL;
    printf("Runtime: save data '%s' restored from its backup\n",r->dir->data);
    return 0;
}
/* sceSaveDataUmountWithBackup(point): unmount, then back the save up. */
static ABI int32_t save_umount_backup(const MountPoint *point) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!point) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    char host[700], meta[700];
    int32_t user=0;
    if (slot>=0) {
        user=slots[slot].user;
        snprintf(host,sizeof(host),"%s",slots[slot].host);
        snprintf(meta,sizeof(meta),"%s",slots[slot].meta);
        runtime_file_unmount(point->data); slots[slot].used=0;
    }
    pthread_mutex_unlock(&lock);
    if (slot<0) return ERR_NOT_FOUND;
    char backup[720];
    snprintf(backup,sizeof(backup),"%s.sce_backup",host);
    int32_t r=make_backup(host,meta,backup);
    const char *dir=strrchr(host,'/');
    if (!r) push_event(1,user,NULL,dir ? dir+1 : host);
    return r;
}
/* sceSaveDataSyncSaveDataMemory(sync): writes reach the file at once; only the event remains. */
static ABI int32_t memory_sync(const MemorySync *sync) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!sync) return ERR_PARAMETER;
    if (sync->slot>=SLOTS || !memories[sync->slot].size) return ERR_MEMORY_NOT_READY;
    push_event(3,sync->user,NULL,"");
    return 0;
}
static ABI int32_t save_event(const void *param, Event *event) {
    (void)param;
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!event) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int found=event_count>0;
    if (found) { *event=events[--event_count]; }
    pthread_mutex_unlock(&lock);
    return found ? 0 : ERR_NOT_FOUND;
}
static ABI int32_t save_progress(float *out) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!out) return ERR_PARAMETER;
    *out=progress; return 0;
}
static ABI int32_t save_clear_progress(void) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    progress=0.0f; return 0;
}
/* sceSaveDataTransferringMount(mount, result): another title's save, read-only. */
static ABI int32_t save_transferring_mount(const TransferringMount *m, MountResult *result) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!m || !m->title || !m->dir) return ERR_PARAMETER;
    return mount(m->user,m->title->data,m->dir,MODE_RDONLY,0,result);
}

/* ---- Queries on a mounted save ---- */
static uint64_t tree_size(const char *path, int depth) {
    struct stat st;
    if (stat(path,&st)) return 0;
    if (!S_ISDIR(st.st_mode)) return (uint64_t)st.st_size;
    if (depth>16) return 0;
    uint64_t total=0;
    DIR *d=opendir(path);
    for (struct dirent *e; d && (e=readdir(d));) {
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        char child[1024];
        if (snprintf(child,sizeof(child),"%s/%s",path,e->d_name)<(int)sizeof(child)) total+=tree_size(child,depth+1);
    }
    if (d) closedir(d);
    return total;
}
/* sceSaveDataGetMountInfo(point, info): the blocks asked for at mount and those still free. */
static ABI int32_t save_mount_info(const MountPoint *point, uint64_t *info) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!point || !info) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    char host[700]; uint64_t blocks=0;
    if (slot>=0) { snprintf(host,sizeof(host),"%s",slots[slot].host); blocks=slots[slot].blocks; }
    pthread_mutex_unlock(&lock);
    if (slot<0) return ERR_NOT_MOUNTED;
    uint64_t used=(tree_size(host,0)+BLOCK_SIZE-1)/BLOCK_SIZE;
    info[0]=blocks; info[1]=used<blocks ? blocks-used : 0;
    return 0;
}
/* sceSaveDataGetParam(point, type, buffer, size, got): 0 all, 1 title, 2 subtitle, 3 detail,
 * 4 user param, 5 modification time. */
static ABI int32_t save_get_param(const MountPoint *point, uint32_t type, void *buffer, uint64_t size, uint64_t *got) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (type>5 || !buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    Param p;
    if (slot>=0) read_param(slots[slot].meta,&p);
    pthread_mutex_unlock(&lock);
    if (slot<0) return ERR_NOT_MOUNTED;
    const char *text=type==1 ? p.title : type==2 ? p.subtitle : p.detail;
    uint64_t n=0;
    switch (type) {
    case 0: if (size<sizeof(Param)) return ERR_PARAMETER; memcpy(buffer,&p,sizeof(p)); n=sizeof(p); break;
    case 1: case 2: case 3:
        if (!size) return ERR_PARAMETER;
        n=strlen(text); if (n>size-1) n=size-1;
        memcpy(buffer,text,n); ((char *)buffer)[n]=0; ++n; break;
    case 4: if (size<4) return ERR_PARAMETER; memcpy(buffer,&p.user_param,4); n=4; break;
    default: if (size<8) return ERR_PARAMETER; memcpy(buffer,&p.mtime,8); n=8; break;
    }
    if (got) *got=n;
    return 0;
}
/* sceSaveDataLoadIcon(point, icon): the mounted save's icon0.png; NOT_FOUND when it has none. */
static ABI int32_t save_load_icon(const MountPoint *point, Icon *icon) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!point || !icon || !icon->buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    char path[720];
    if (slot>=0) snprintf(path,sizeof(path),"%s/icon0.png",slots[slot].meta);
    pthread_mutex_unlock(&lock);
    if (slot<0) return ERR_NOT_MOUNTED;
    return load_icon(path,icon);
}

/* THEATRHYTHM_SAVE_TRACE=1: every call above with its result (polls only when the result changes). */
static int save_trace(void) { static int v=-1; if (v<0) { const char *e=getenv("THEATRHYTHM_SAVE_TRACE"); v=e && e[0]=='1'; } return v; }
static int32_t traced(const char *call, const char *detail, int32_t r) {
    if (save_trace()) printf("Save trace: %s(%s) -> 0x%x\n",call,detail ? detail : "",(unsigned)r);
    return r;
}
static const char *point_name(const MountPoint *p) { return p ? p->data : "null"; }
static const char *dir_name(const DirName *d) { return d ? d->data : "null"; }
static ABI int32_t t_initialize(const void *p) { return traced("Initialize",NULL,save_initialize(p)); }
static ABI int32_t t_terminate(void) { return traced("Terminate",NULL,save_terminate()); }
static ABI int32_t t_mount(const Mount1 *m, MountResult *r) {
    char d[64]; snprintf(d,sizeof(d),"%s, mode 0x%x",m ? dir_name(m->dir) : "null",m ? m->mode : 0);
    return traced("Mount",d,save_mount(m,r));
}
static ABI int32_t t_mount2(const Mount2 *m, MountResult *r) {
    char d[64]; snprintf(d,sizeof(d),"%s, mode 0x%x",m ? dir_name(m->dir) : "null",m ? m->mode : 0);
    return traced("Mount2",d,save_mount2(m,r));
}
static ABI int32_t t_umount(const MountPoint *p) { return traced("Umount",point_name(p),save_umount(p)); }
static ABI int32_t t_set_param(const MountPoint *p, uint32_t type, const void *b, uint64_t n) {
    char d[48]; snprintf(d,sizeof(d),"%s, type %u",point_name(p),type);
    return traced("SetParam",d,save_set_param(p,type,b,n));
}
static ABI int32_t t_icon(const MountPoint *p, const Icon *i) { return traced("SaveIcon",point_name(p),save_icon(p,i)); }
static ABI int32_t t_delete(const Delete *d) { return traced("Delete",d ? dir_name(d->dir) : "null",save_delete(d)); }
static ABI int32_t t_search(const SearchCond *c, SearchResult *r) {
    int32_t x=save_search(c,r);
    char d[64]; snprintf(d,sizeof(d),"%s: %u hits",c ? dir_name(c->dir) : "null",!x && r ? r->hits : 0);
    return traced("DirNameSearch",d,x);
}
static ABI int32_t t_backup(const BackupRequest *b) { return traced("Backup",b ? dir_name(b->dir) : "null",save_backup(b)); }
static ABI int32_t t_check_backup(const CheckBackup *c) { return traced("CheckBackupData",c ? dir_name(c->dir) : "null",save_check_backup(c)); }
static ABI int32_t t_restore_backup(const RestoreBackup *r) { return traced("RestoreBackupData",r ? dir_name(r->dir) : "null",save_restore_backup(r)); }
static ABI int32_t t_umount_backup(const MountPoint *p) { return traced("UmountWithBackup",point_name(p),save_umount_backup(p)); }
static ABI int32_t t_event(const void *p, Event *e) {
    static int32_t last=1;
    int32_t r=save_event(p,e);
    if (r==last) return r;
    last=r;
    char d[48]; snprintf(d,sizeof(d),"type %u",!r && e ? e->type : 0);
    return traced("GetEventResult",d,r);
}
static ABI int32_t t_transferring(const TransferringMount *m, MountResult *r) { return traced("TransferringMount",m ? dir_name(m->dir) : "null",save_transferring_mount(m,r)); }
static ABI int32_t t_mount_info(const MountPoint *p, uint64_t *i) { return traced("GetMountInfo",point_name(p),save_mount_info(p,i)); }
static ABI int32_t t_get_param(const MountPoint *p, uint32_t type, void *b, uint64_t n, uint64_t *got) {
    char d[48]; snprintf(d,sizeof(d),"%s, type %u",point_name(p),type);
    return traced("GetParam",d,save_get_param(p,type,b,n,got));
}
static ABI int32_t t_load_icon(const MountPoint *p, Icon *i) { return traced("LoadIcon",point_name(p),save_load_icon(p,i)); }

static const RuntimeExport exports[]={
    {"sceSaveDataInitialize",t_initialize}, {"sceSaveDataInitialize2",t_initialize},
    {"sceSaveDataInitialize3",t_initialize}, {"sceSaveDataTerminate",t_terminate},
    {"sceSaveDataMount",t_mount}, {"sceSaveDataMount2",t_mount2}, {"sceSaveDataUmount",t_umount},
    {"sceSaveDataSetParam",t_set_param}, {"sceSaveDataSaveIcon",t_icon}, {"sceSaveDataDelete",t_delete},
    {"sceSaveDataDirNameSearch",t_search},
    {"sceSaveDataSetupSaveDataMemory",memory_setup}, {"sceSaveDataGetSaveDataMemory",memory_get},
    {"sceSaveDataSetSaveDataMemory",memory_set},
    {"sceSaveDataSetupSaveDataMemory2",memory_setup2}, {"sceSaveDataGetSaveDataMemory2",memory_get2},
    {"sceSaveDataSetSaveDataMemory2",memory_set2}, {"sceSaveDataSyncSaveDataMemory",memory_sync},
    {"sceSaveDataBackup",t_backup}, {"sceSaveDataCheckBackupData",t_check_backup},
    {"sceSaveDataRestoreBackupData",t_restore_backup}, {"sceSaveDataUmountWithBackup",t_umount_backup},
    {"sceSaveDataGetEventResult",t_event}, {"sceSaveDataGetProgress",save_progress},
    {"sceSaveDataClearProgress",save_clear_progress}, {"sceSaveDataTransferringMount",t_transferring},
    {"sceSaveDataGetMountInfo",t_mount_info}, {"sceSaveDataGetParam",t_get_param},
    {"sceSaveDataLoadIcon",t_load_icon},
};
uintptr_t runtime_savedata_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
void runtime_savedata_report(void) { printf("Runtime: save data mounts=%zu, memory writes=%zu, backups=%zu\n",mounts_done,memory_writes,backups_done); }

