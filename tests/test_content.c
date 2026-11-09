/* Add-ons: the save folder's addcont/ and an extra DLC folder (shadPS4's addcont, with its title ID
 * folder, or a plain folder of add-ons) are listed together; the save folder's copy of a label wins.
 * The game folder is never an add-on, nor are its own folders when it is the extra DLC folder. */
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#include "../src/runtime_content.c"
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

static char user_dir[256], mounted[16][700];
const char *runtime_file_user_dir(void) { return user_dir; }
int runtime_file_mount(const char *guest, const char *host) {
    int i=atoi(guest+strlen("/addcont"));
    if (!strncmp(guest,"/addcont",8) && i>=0 && i<16) snprintf(mounted[i],sizeof(mounted[i]),"%s",host);
    return 0;
}
void runtime_file_unmount(const char *point) { (void)point; }
int32_t runtime_module_start(const char *file, uint64_t args, const void *argp, int32_t *result) {
    (void)file; (void)args; (void)argp; (void)result; return 0;
}

static void folder(const char *path) { assert(!mkdir(path,0755) || errno==EEXIST); }
static void file(const char *path) { FILE *f=fopen(path,"wb"); assert(f); fputs("x",f); fclose(f); }

/* The labels the game is given, comma-separated. */
static void listed(char *out, size_t size) {
    unsigned char list[24*8]; uint32_t hits=0;
    assert(!addon_list(0,list,8,&hits));
    out[0]=0;
    for (uint32_t i=0;i<hits;++i)
        snprintf(out+strlen(out),size-strlen(out),"%s%s",i ? "," : "",(const char *)list+24*i);
}
static void rescan(void) { addon_count=0; addons_scanned=0; memset(mounted,0,sizeof(mounted)); }

int main(void) {
    const char *tmp=getenv("TMP");
    if (!tmp) tmp=getenv("TEMP");
    if (!tmp) tmp="/tmp";
    char root[256], path[700], text[256], mount[16];
    snprintf(root,sizeof(root),"%s/theatrhythm-content-test-%d",tmp,(int)getpid());
    folder(root);
    snprintf(user_dir,sizeof(user_dir),"%s/save",root); folder(user_dir);
    snprintf(path,sizeof(path),"%s/addcont",user_dir); folder(path);
    snprintf(path,sizeof(path),"%s/addcont/SAVEONLY",user_dir); folder(path);
    snprintf(path,sizeof(path),"%s/addcont/SHARED",user_dir); folder(path);
    /* shadPS4's layout: <addcont>/<title ID>/<label>/ */
    snprintf(path,sizeof(path),"%s/shadps4",root); folder(path);
    snprintf(path,sizeof(path),"%s/shadps4/CUSA32853",root); folder(path);
    snprintf(path,sizeof(path),"%s/shadps4/CUSA32853/EXTRA",root); folder(path);
    snprintf(path,sizeof(path),"%s/shadps4/CUSA32853/SHARED",root); folder(path);
    snprintf(path,sizeof(path),"%s/shadps4/CUSA32853/notes.txt",root); file(path);
    snprintf(path,sizeof(path),"%s/shadps4/CUSA32853/LABEL_LONGER_THAN_16",root); folder(path);
    /* A game dump with its DLC kept inside it, in a folder of dumps (and of other DLC). */
    char game[300];
    snprintf(path,sizeof(path),"%s/dumps",root); folder(path);
    snprintf(game,sizeof(game),"%s/dumps/CUSA32853",root); folder(game);
    const char *const parts[]={"Media","sce_module","sce_sys","DLCPACK000000000"};
    for (size_t i=0;i<sizeof(parts)/sizeof(*parts);++i) { snprintf(path,sizeof(path),"%s/%s",game,parts[i]); folder(path); }
    snprintf(path,sizeof(path),"%s/dumps/Theatrhythm",root); folder(path); /* the game under another name */
    snprintf(path,sizeof(path),"%s/dumps/Theatrhythm/Media",root); folder(path);
    snprintf(path,sizeof(path),"%s/dumps/OTHERDLC",root); folder(path);

    uint32_t profile[5]={1,0,0,0,0};
    unsigned char init[32]={0}, boot[40];
    runtime_content_configure(profile);
    assert(!module_load(0xb4));
    assert(!content_init(init,boot));

    /* No extra folder: only the save folder's add-ons. */
    listed(text,sizeof(text));
    assert(!strcmp(text,"SAVEONLY,SHARED"));

    /* shadPS4's addcont: its CUSA32853 folder is the one read. */
    rescan();
    snprintf(path,sizeof(path),"%s/shadps4",root);
    runtime_content_extra_dir(path,"CUSA32853",game);
    listed(text,sizeof(text));
    assert(!strcmp(text,"EXTRA,SAVEONLY,SHARED"));
    assert(!addon_mount(0,"EXTRA",mount) && !strcmp(mount,"/addcont0"));
    snprintf(path,sizeof(path),"%s/shadps4/CUSA32853/EXTRA",root);
    assert(!strcmp(mounted[0],path));
    assert(!addon_mount(0,"SHARED",mount) && !strcmp(mount,"/addcont2"));
    snprintf(path,sizeof(path),"%s/addcont/SHARED",user_dir);
    assert(!strcmp(mounted[2],path));
    assert(addon_mount(0,"MISSING",mount)==NOT_FOUND);

    /* The title ID folder itself works the same. */
    rescan();
    snprintf(path,sizeof(path),"%s/shadps4/CUSA32853",root);
    runtime_content_extra_dir(path,"CUSA32853",game);
    listed(text,sizeof(text));
    assert(!strcmp(text,"EXTRA,SAVEONLY,SHARED"));

    /* A folder that is gone lists only the save folder's add-ons. */
    rescan();
    snprintf(path,sizeof(path),"%s/gone",root);
    runtime_content_extra_dir(path,"CUSA32853",game);
    listed(text,sizeof(text));
    assert(!strcmp(text,"SAVEONLY,SHARED"));

    /* The game folder as the extra DLC folder (Windows: in another spelling): only its DLC. */
    rescan();
    runtime_content_extra_dir(game,"CUSA32853",game);
    listed(text,sizeof(text));
    assert(!strcmp(text,"DLCPACK000000000,SAVEONLY,SHARED"));
#ifdef _WIN32
    rescan();
    snprintf(path,sizeof(path),"%s\\DUMPS\\cusa32853\\",root);
    runtime_content_extra_dir(path,NULL,game);
    listed(text,sizeof(text));
    assert(!strcmp(text,"DLCPACK000000000,SAVEONLY,SHARED"));
#endif

    /* A folder of dumps: its CUSA32853 folder is the game, so the same. */
    rescan();
    snprintf(path,sizeof(path),"%s/dumps",root);
    runtime_content_extra_dir(path,"CUSA32853",game);
    listed(text,sizeof(text));
    assert(!strcmp(text,"DLCPACK000000000,SAVEONLY,SHARED"));

    /* Without a title ID folder, the game folder among the add-ons is left out. */
    rescan();
    snprintf(path,sizeof(path),"%s/dumps/Theatrhythm",root);
    char other[300]; snprintf(other,sizeof(other),"%s/dumps",root);
    runtime_content_extra_dir(other,NULL,path);
    listed(text,sizeof(text));
    assert(!strcmp(text,"CUSA32853,OTHERDLC,SAVEONLY,SHARED"));

    const char *const made[]={"dumps/CUSA32853/Media","dumps/CUSA32853/sce_module","dumps/CUSA32853/sce_sys",
                              "dumps/CUSA32853/DLCPACK000000000","dumps/CUSA32853","dumps/Theatrhythm/Media",
                              "dumps/Theatrhythm","dumps/OTHERDLC","dumps","shadps4/CUSA32853/EXTRA","shadps4/CUSA32853/SHARED","shadps4/CUSA32853/LABEL_LONGER_THAN_16",
                              "shadps4/CUSA32853","shadps4","save/addcont/SAVEONLY","save/addcont/SHARED","save/addcont","save",""};
    snprintf(path,sizeof(path),"%s/shadps4/CUSA32853/notes.txt",root); remove(path);
    for (size_t i=0;i<sizeof(made)/sizeof(*made);++i) {
        snprintf(path,sizeof(path),"%s/%s",root,made[i]); rmdir(path);
    }
    puts("content test: ok");
    return 0;
}
