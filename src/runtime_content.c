/* Offline AppContent provider for configured and locally installed add-ons.
 * No downloads or license emulation. */
#include "runtime.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define INVALID_ID ((int32_t)0x805a1000)
#define NOT_LOADED ((int32_t)0x805a1001)
#define PARAMETER ((int32_t)0x80d90002)
#define BUSY ((int32_t)0x80d90003)
#define NOT_FOUND ((int32_t)0x80D90005)
#define INSTALLED 4
#define ADDON_INFO_SIZE 24
static unsigned refs, initialized, configured;
static uint32_t parameters[5];
static unsigned queries, lists;
static RuntimeContentAddon addons[RUNTIME_CONTENT_MAX_ADDONS];
static size_t addon_count;
static char app0[512];
void runtime_content_configure(const uint32_t values[5],const RuntimeContentAddon *items,size_t count,const char *root) {
    if (refs || initialized || (values[0]!=1 && values[0]!=3) || count>RUNTIME_CONTENT_MAX_ADDONS ||
        (count && (!items || !root || !*root)) || (root && strlen(root)>=sizeof(app0))) {
        fputs("ERROR: invalid or late content profile\n",stderr); exit(1);
    }
    for (size_t i=0;i<count;++i) {
        if (!memchr(items[i].entitlement,0,sizeof(items[i].entitlement)) ||
            !items[i].entitlement[0] ||
            !memchr(items[i].directory,0,sizeof(items[i].directory)) ||
            !items[i].directory[0] || strchr(items[i].directory,'/') ||
            strchr(items[i].directory,'\\') || !strcmp(items[i].directory,".") ||
            !strcmp(items[i].directory,"..")) {
            fputs("ERROR: invalid add-on in content profile\n",stderr); exit(1);
        }
        for (size_t j=0;j<i;++j) if (!strcmp(items[i].entitlement,items[j].entitlement)) {
            fputs("ERROR: duplicate add-on in content profile\n",stderr); exit(1);
        }
    }
    memcpy(parameters,values,sizeof(parameters));
    if (count) memcpy(addons,items,count*sizeof(*addons));
    addon_count=count;
    if (root) memcpy(app0,root,strlen(root)+1);
    configured=1;
}
static void require_provider(void) {
    if (!configured) { fputs("STOP: AppContent needs --content-profile\n",stderr); exit(21); }
}
/* System modules: every library this eboot imports is provided by the host
 * runtime, so loading one only tracks a reference count. AppContent (0xb4)
 * additionally requires the configured content profile. */
static unsigned module_refs[0x10000];
/* Game-shipped system modules (shadPS4 sysmodule_table.h, flag IsGame): the first load starts
 * /app0/sce_module/<name>.prx, after the game module it needs, if any. */
static const struct { uint16_t id; const char *file, *needs; } game_modules[]={
    {0x38,"libSceFace.prx",NULL}, {0x86,"libSceS3DConversion.prx",NULL}, {0x93,"libSceHand.prx",NULL},
    {0xcb,"libSceHeadTracker.prx",NULL}, {0xd8,"libSceFaceTracker.prx","libSceFace.prx"},
    {0xd9,"libSceHandTracker.prx",NULL}, {0xe8,"libSceAudioLatencyEstimation.prx",NULL},
    {0xfe,"libSceJobManager.prx",NULL}, {0x102,"libSceNpToolkit2.prx",NULL},
};
static int32_t start_game_module(uint16_t id) {
    for (size_t i=0;i<sizeof(game_modules)/sizeof(*game_modules);++i) {
        if (game_modules[i].id!=id) continue;
        int32_t r=game_modules[i].needs ? runtime_module_start(game_modules[i].needs,0,NULL,NULL) : 0;
        if (r>=0) r=runtime_module_start(game_modules[i].file,0,NULL,NULL);
        return r<0 ? r : 0;
    }
    return 0;
}
static ABI int32_t module_load(uint16_t id) {
    if (!id) return INVALID_ID;
    if (id==0xb4) require_provider();
    if (module_refs[id]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); exit(21); }
    if (!module_refs[id]) { int32_t r=start_game_module(id); if (r<0) return r; }
    ++module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; printf("Runtime: AppContent host provider loaded; references=%u\n",refs); }
    else printf("Runtime: system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
static ABI int32_t module_loaded(uint16_t id) {
    if (!id) return INVALID_ID;
    return module_refs[id] ? 0 : NOT_LOADED;
}
static ABI int32_t module_unload(uint16_t id) {
    int32_t result=module_loaded(id);
    if (result) return result;
    --module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; if (!refs) initialized=0; }
    return 0;
}
/* sceSysmoduleLoadModuleInternalWithArg(id, argc, argv, unk, result): internal module ids
 * (shadPS4 sysmodule_internal.cpp). Modules this runtime implements need no start routine,
 * so a load counts a reference and reports a start result of 0. */
static unsigned internal_refs[0x10000];
static ABI int32_t module_load_internal_arg(uint32_t id,int32_t argc,const void *argv,uint64_t unk,int32_t *result) {
    (void)argc; (void)argv;
    if (!(id&0x7fffffff) || id&0x7fff0000 || unk) return INVALID_ID;
    if (internal_refs[id&0xffff]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); exit(21); }
    ++internal_refs[id&0xffff];
    if (result) *result=0;
    printf("Runtime: internal system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
static void require_loaded(void) {
    require_provider();
    if (!refs) { fputs("STOP: AppContent called before module load\n",stderr); exit(21); }
}
static ABI int32_t content_init(const unsigned char *init,unsigned char *boot) {
    require_loaded();
    if (!init || !boot) return PARAMETER;
    if (initialized) return BUSY;
    /* Only the reserved-zero init profile is implemented. */
    for (unsigned i=0;i<32;++i) if (init[i]) return PARAMETER;
    memset(boot,0,40); initialized=1;
    puts("Runtime: AppContent initialized; offline base-game profile");
    return 0;
}
static void require_initialized(void) {
    require_loaded();
    if (!initialized) { fputs("STOP: AppContent query before initialization\n",stderr); exit(21); }
}
static ABI int32_t param_int(uint32_t id,int32_t *out) {
    require_initialized();
    if (id>4 || !out) return PARAMETER;
    memcpy(out,&parameters[id],4); ++queries;
    printf("Runtime: AppContent parameter %u = %d\n",id,*out);
    return 0;
}
/* Installed add-ons: each folder <user>/addcont/<label>/ is one, its name the entitlement label
 * (up to 16 characters), listed in name order and mounted as /addcont<index>. */
#define LABEL_SIZE 17
#define MAX_ADDONS 64
static char installed_addons[MAX_ADDONS][LABEL_SIZE];
static unsigned installed_addon_count, addons_scanned;
static int addon_order(const void *a,const void *b) { return strcmp(a,b); }
static void scan_addons(void) {
    if (addons_scanned) return;
    addons_scanned=1;
    char path[600]; snprintf(path,sizeof(path),"%s/addcont",runtime_file_user_dir());
    DIR *d=opendir(path);
    if (!d) return;
    for (struct dirent *e; installed_addon_count<MAX_ADDONS && (e=readdir(d));) {
        size_t n=strlen(e->d_name);
        if (e->d_name[0]=='.' || n>=LABEL_SIZE) continue;
        size_t i;
        for (i=0;i<addon_count;++i) if (!strcmp(addons[i].entitlement,e->d_name)) break;
        if (i<addon_count) continue;
        char folder[700]; snprintf(folder,sizeof(folder),"%s/%s",path,e->d_name);
        DIR *sub=opendir(folder);
        if (!sub) continue;
        closedir(sub);
        memcpy(installed_addons[installed_addon_count++],e->d_name,n+1);
    }
    closedir(d);
    qsort(installed_addons,installed_addon_count,LABEL_SIZE,addon_order);
    for (unsigned i=0;i<installed_addon_count;++i)
        printf("Runtime: add-on %s installed (%s/%s)\n",installed_addons[i],path,installed_addons[i]);
}
static int find_installed_addon(const char *label) {
    for (unsigned i=0;i<installed_addon_count;++i)
        if (!strncmp(label,installed_addons[i],LABEL_SIZE-1)) return (int)i;
    return -1;
}
/* sceAppContentGetAddcontInfoList(service, list, capacity, hits): OrbisAppContentAddcontInfo is
 * the entitlement label (17 bytes, 3 padding) and the download status (4 = installed). A null
 * list only counts. Never touch unused list slots. */
static ABI int32_t addon_list(uint32_t service,void *list,uint32_t capacity,uint32_t *hits) {
    require_initialized();
    if (service>7) return PARAMETER;
    if ((!capacity || !list) && !hits) return PARAMETER;
    scan_addons();
    size_t count=service ? 0 : addon_count+installed_addon_count;
    if (list) {
        size_t written=capacity<count ? capacity : count;
        for (size_t i=0;i<written;++i) {
            const char *label=i<addon_count ? addons[i].entitlement : installed_addons[i-addon_count];
            unsigned char *entry=(unsigned char *)list+i*ADDON_INFO_SIZE;
            memset(entry,0,ADDON_INFO_SIZE);
            memcpy(entry,label,strlen(label));
            uint32_t status=INSTALLED;
            memcpy(entry+20,&status,sizeof(status));
        }
        count=written;
    }
    if (hits) *hits=(uint32_t)count;
    printf("Runtime: AppContent add-on list service=%u, hits=%zu\n",service,count);
    ++lists; return 0;
}
/* Temporary data (shadPS4 app_content.cpp): sceAppContentTemporaryDataMount2(option, mount)
 * names /temp0, which the file layer keeps in the user directory; format and unmount have
 * nothing to do. Free space for temporary and download data is reported as 1 GiB, in KiB. */
static ABI int32_t temp_mount(uint32_t option,char *mount) {
    (void)option;
    if (!mount) return PARAMETER;
    memset(mount,0,16); memcpy(mount,"/temp0",6); return 0;
}
static ABI int32_t content_nothing(void) { return 0; }
static ABI int32_t free_space_kb(const char *mount,uint64_t *kb) {
    (void)mount;
    if (!kb) return PARAMETER;
    *kb=1048576; return 0;
}
/* Files under <add-on>/StreamingAssets/ are served in place of (or next to) the game's
 * Media/StreamingAssets/ files of the same relative path, at both of the game's roots: plain files
 * such as an Event stage's Movie/EMS/MUSICnnnnnnn.mp4 are not Addressables assets. */
static void overlay_streaming_assets(const char *host,const char *relative) {
    static const char *const roots[]={"/app0/Media/StreamingAssets","/archive/mount/point/Media/StreamingAssets"};
    char path[700]; snprintf(path,sizeof(path),"%s/StreamingAssets%s",host,relative);
    DIR *d=opendir(path);
    if (!d) {
        for (size_t i=0;i<sizeof(roots)/sizeof(*roots);++i) {
            char guest[256]; snprintf(guest,sizeof(guest),"%s%s",roots[i],relative);
            if (runtime_file_mount(guest,path)) { printf("Runtime: add-on file %s not mounted (mount table full)\n",guest); return; }
        }
        printf("Runtime: add-on file Media/StreamingAssets%s\n",relative);
        return;
    }
    for (struct dirent *e; (e=readdir(d));) {
        if (e->d_name[0]=='.') continue;
        char next[512]; snprintf(next,sizeof(next),"%s/%s",relative,e->d_name);
        overlay_streaming_assets(host,next);
    }
    closedir(d);
}
/* sceAppContentAddcontMount(service, entitlement, mount): /addcont<index> (16-byte mount point)
 * names the add-on's folder; NOT_FOUND for a label that is not installed. */
static ABI int32_t addon_mount(uint32_t service,const char *label,char *mount) {
    require_initialized();
    if (service>7 || !label || !mount) return PARAMETER;
    if (service) return NOT_FOUND;
    scan_addons();
    size_t index=0;
    char host[700];
    for (;index<addon_count;++index)
        if (!strncmp(label,addons[index].entitlement,RUNTIME_CONTENT_LABEL_SIZE)) break;
    if (index<addon_count) {
        int n=snprintf(host,sizeof(host),"%s/DLC/%s",app0,addons[index].directory);
        if (n<0 || (size_t)n>=sizeof(host)) return PARAMETER;
    } else {
        int installed=find_installed_addon(label);
        if (installed<0) return NOT_FOUND;
        index=addon_count+(size_t)installed;
        int n=snprintf(host,sizeof(host),"%s/addcont/%s",runtime_file_user_dir(),installed_addons[installed]);
        if (n<0 || (size_t)n>=sizeof(host)) return NOT_FOUND;
    }
    char guest[16]; snprintf(guest,sizeof(guest),"/addcont%zu",index);
    if (runtime_file_mount(guest,host)) {
        if (index<addon_count) {
            fprintf(stderr,"ERROR: cannot mount add-on %s\n",addons[index].entitlement);
            return PARAMETER;
        }
        return NOT_FOUND;
    }
    char assets[720]; snprintf(assets,sizeof(assets),"%s/StreamingAssets",host);
    DIR *d=opendir(assets);
    if (d) { closedir(d); overlay_streaming_assets(host,""); }
    memset(mount,0,16); memcpy(mount,guest,strlen(guest));
    printf("Runtime: add-on %s mounted at %s\n",
           index<addon_count ? addons[index].entitlement : installed_addons[index-addon_count],guest);
    return 0;
}
/* sceAppContentAddcontUnmount(mount): drops the mount; the folder stays listed. */
static ABI int32_t addon_unmount(const char *mount) {
    require_initialized();
    if (!mount) return PARAMETER;
    scan_addons();
    size_t count=addon_count+installed_addon_count;
    for (size_t i=0;i<count;++i) {
        char expected[16];
        snprintf(expected,sizeof(expected),"/addcont%zu",i);
        if (!strncmp(mount,expected,sizeof(expected))) {
            runtime_file_unmount(expected);
            return 0;
        }
    }
    return PARAMETER;
}
uintptr_t runtime_content_resolve(const char *name) {
    if (!strcmp(name,"buYbeLOGWmA#libSceAppContent")) return (uintptr_t)temp_mount;
    if (!strcmp(name,"a5N7lAG0y2Q#libSceAppContent")) return (uintptr_t)content_nothing; /* TemporaryDataFormat */
    if (!strcmp(name,"bcolXMmp6qQ#libSceAppContent")) return (uintptr_t)content_nothing; /* TemporaryDataUnmount */
    if (!strcmp(name,"3rHWaV-1KC4#libSceAppContent")) return (uintptr_t)addon_unmount;
    if (!strcmp(name,"SaKib2Ug0yI#libSceAppContent")) return (uintptr_t)free_space_kb;   /* TemporaryData */
    if (!strcmp(name,"Gl6w5i0JokY#libSceAppContent")) return (uintptr_t)free_space_kb;   /* DownloadData */
    if (!strcmp(name,"VANhIWcqYak#libSceAppContent")) return (uintptr_t)addon_mount;
    if (!strcmp(name,"g8cM39EUZ6o#libSceSysmodule")) return (uintptr_t)module_load;
    if (!strcmp(name,"fMP5NHUOaMk#libSceSysmodule")) return (uintptr_t)module_loaded;
    if (!strcmp(name,"eR2bZFAAU0Q#libSceSysmodule")) return (uintptr_t)module_unload;
    if (!strcmp(name,"hHrGoGoNf+s#libSceSysmodule")) return (uintptr_t)module_load_internal_arg;
    if (!strcmp(name,"R9lA82OraNs#libSceAppContent")) return (uintptr_t)content_init;
    if (!strcmp(name,"99b82IKXpH4#libSceAppContent")) return (uintptr_t)param_int;
    if (!strcmp(name,"xnd8BJzAxmk#libSceAppContent")) return (uintptr_t)addon_list;
    return 0;
}
void runtime_content_report(void) {
    printf("Runtime: AppContent references=%u, initialized=%u, parameter queries=%u, list queries=%u\n",refs,initialized,queries,lists);
}
