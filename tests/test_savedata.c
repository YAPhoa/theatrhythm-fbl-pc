/* Save data: when a save was last written (the load list's focus and the menu's last autosave). */
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#include "../src/runtime_savedata.c"
#include <assert.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <utime.h>
#endif

static char user_dir[256];
const char *runtime_file_user_dir(void) { return user_dir; }
int runtime_file_mount(const char *guest, const char *host) { (void)guest; (void)host; return 0; }
void runtime_file_unmount(const char *point) { (void)point; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) { (void)table; (void)count; (void)nid; return 0; }

static void touch(const char *path, time_t when) {
    FILE *f=fopen(path,"wb"); assert(f); fputs("x",f); fclose(f);
#ifdef _WIN32
    HANDLE h=CreateFileA(path,FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    assert(h!=INVALID_HANDLE_VALUE);
    uint64_t ft=((uint64_t)when+11644473600ULL)*10000000ULL;
    FILETIME filetime={(DWORD)ft,(DWORD)(ft>>32)};
    assert(SetFileTime(h,NULL,NULL,&filetime));
    CloseHandle(h);
#else
    struct utimbuf times={when,when}; assert(!utime(path,&times));
#endif
}

#ifdef _WIN32
static int touch_dir(const char *path, time_t when) {
    HANDLE h=CreateFileA(path,FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
    if (h==INVALID_HANDLE_VALUE) return -1;
    uint64_t ft=((uint64_t)when+11644473600ULL)*10000000ULL;
    FILETIME filetime={(DWORD)ft,(DWORD)(ft>>32)};
    BOOL ok=SetFileTime(h,NULL,NULL,&filetime);
    CloseHandle(h);
    return ok ? 0 : -1;
}
#else
static int touch_dir(const char *path, time_t when) {
    struct utimbuf times={when,when}; return utime(path,&times);
}
#endif

int main(void) {
    const char *tmp=getenv("TMP");
    if (!tmp) tmp=getenv("TEMP");
    if (!tmp) tmp="/tmp";
    snprintf(user_dir,sizeof(user_dir),"%s/theatrhythm-savedata-test-%d",tmp,(int)getpid());
    runtime_savedata_configure("CUSA32853");
    assert(runtime_savedata_mtime(1,"AUTOSAVE")==0); /* no saves yet */

    assert(save_initialize(NULL)==0);
    DirName autosave={"AUTOSAVE"}, manual={"SAVE0001"};
    MountResult result;
    assert(mount(1,NULL,&autosave,MODE_CREATE2,0,&result)==0 && save_umount(&result.point)==0);
    assert(mount(1,NULL,&manual,MODE_CREATE2,0,&result)==0 && save_umount(&result.point)==0);

    /* A save's time is the newest of its files and its param.bin, whatever the folder's own time. */
    char base[600], path[800];
    root(1,NULL,base,sizeof(base));
    const time_t savedata_time=time(NULL)-10000;
    snprintf(path,sizeof(path),"%s/AUTOSAVE.sce_sys/param.bin",base); touch(path,savedata_time);
    snprintf(path,sizeof(path),"%s/SAVE0001.sce_sys/param.bin",base); touch(path,savedata_time+500);
    snprintf(path,sizeof(path),"%s/SAVE0001",base); assert(!touch_dir(path,savedata_time));
    snprintf(path,sizeof(path),"%s/AUTOSAVE/data.bin",base); touch(path,savedata_time+1000);
    snprintf(path,sizeof(path),"%s/AUTOSAVE",base); assert(!touch_dir(path,savedata_time));
    assert(runtime_savedata_mtime(1,"AUTOSAVE")==savedata_time+1000 &&
           runtime_savedata_mtime(1,"SAVE0001")==savedata_time+500);
    assert(runtime_savedata_mtime(1,"MISSING")==0 && runtime_savedata_mtime(1,"../x")==0);

    /* A directory touched with a newer timestamp must NOT override the files inside. */
    snprintf(path,sizeof(path),"%s/SAVE0001",base); assert(!touch_dir(path,savedata_time+4000));
    assert(runtime_savedata_mtime(1,"SAVE0001")==savedata_time+500);

    /* An empty save directory without any files or param.bin returns 0 even if directory mtime > 0. */
    snprintf(path,sizeof(path),"%s/SAVEEMPTY",base);
    assert(!make_dirs(path));
    assert(!touch_dir(path,savedata_time+3000));
    assert(runtime_savedata_mtime(1,"SAVEEMPTY")==0);

#ifndef _WIN32
    /* Symlinks pointing outside the save directory are not followed. */
    snprintf(path,sizeof(path),"%s/external.bin",user_dir); touch(path,savedata_time+99999);
    char link_path[800]; snprintf(link_path,sizeof(link_path),"%s/SAVE0001/symlink.bin",base);
    assert(!symlink(path,link_path));
    assert(runtime_savedata_mtime(1,"SAVE0001")==savedata_time+500);
    assert(!unlink(link_path));
#endif

    /* Backups: made of an unmounted save, found, restored over later changes; events and progress. */
    BackupRequest request={.user=1,.dir=&manual};
    Param titled={.title="Chapter 1"};
    snprintf(path,sizeof(path),"%s/SAVE0001.sce_sys",base); write_param(path,&titled);
    snprintf(path,sizeof(path),"%s/SAVE0001/slot.bin",base); touch(path,savedata_time+1700);
    assert(save_backup(&request)==0);
    float done=0; Event event;
    assert(save_progress(&done)==0 && done==1.0f && save_event(NULL,&event)==0 && event.type==2 && !strcmp(event.dir.data,"SAVE0001"));
    assert(save_event(NULL,&event)==ERR_NOT_FOUND && save_clear_progress()==0 && save_progress(&done)==0 && done==0.0f);
    remove(path);
    Param checked;
    CheckBackup check={.user=1,.dir=&manual,.param=&checked};
    assert(save_check_backup(&check)==0 && !strcmp(checked.title,"Chapter 1"));
    RestoreBackup restore={.user=1,.dir=&manual};
    assert(save_restore_backup(&restore)==0);
    struct stat st; assert(!stat(path,&st));
    CheckBackup missing={.user=1,.dir=&autosave};
    assert(save_check_backup(&missing)==ERR_NOT_FOUND);

    /* A mounted save: busy for backups; its blocks, params and icon; unmounting with a backup. */
    Mount2 m2={.user=1,.dir=&manual,.blocks=100,.mode=0};
    assert(save_mount2(&m2,&result)==0 && save_backup(&request)==ERR_BUSY);
    uint64_t info[6];
    assert(save_mount_info(&result.point,info)==0 && info[0]==100 && info[1]==99);
    assert(save_set_param(&result.point,1,"Chapter 2",9)==0);
    char text[64]; uint64_t got=0;
    assert(save_get_param(&result.point,1,text,sizeof(text),&got)==0 && !strcmp(text,"Chapter 2") && got==10);
    unsigned char png[4]={1,2,3,4}, read_back[8]={0};
    Icon icon={.buffer=png,.buffer_size=4,.data_size=4}, loaded={.buffer=read_back,.buffer_size=sizeof(read_back)};
    assert(save_load_icon(&result.point,&loaded)==ERR_NOT_FOUND);
    assert(save_icon(&result.point,&icon)==0 && save_load_icon(&result.point,&loaded)==0 && loaded.data_size==4 && read_back[3]==4);
    assert(save_umount_backup(&result.point)==0 && save_event(NULL,&event)==0 && event.type==1 && event.user==1);

    /* SaveDataMemory v2: created once (existed 0), then kept (existed = its size); slot data. */
    MemorySetup2 setup={.user=1,.size=64,.slot=1};
    MemorySetupResult setup_result={0};
    assert(memory_setup2(&setup,&setup_result)==0 && setup_result.existed==0);
    assert(memory_setup2(&setup,&setup_result)==0 && setup_result.existed==64);
    char blob[8]="score", back[8]={0};
    MemoryData put={.buffer=blob,.size=6,.offset=8}, take={.buffer=back,.size=6,.offset=8};
    MemorySet2 set={.user=1,.data=&put,.slot=1};
    MemoryGet2 get={.user=1,.data=&take,.slot=1};
    assert(memory_set2(&set)==0 && memory_get2(&get)==0 && !strcmp(back,"score"));
    MemorySync sync={.user=1,.slot=1};
    MemoryGet2 unready={.user=1,.slot=2};
    assert(memory_sync(&sync)==0 && memory_get2(&unready)==ERR_MEMORY_NOT_READY);

    remove_tree(user_dir);
    puts("savedata test: ok");
    return 0;
}
