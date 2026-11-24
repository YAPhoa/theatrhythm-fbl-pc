/* Guest paths (src/runtime_file.c): mounts map a guest prefix to a host folder, and a ".."
 * component is refused whichever separator it is written with. */
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#include "../src/runtime_file.c"
#include <assert.h>

/* runtime.c pieces runtime_file.c uses. */
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) { (void)table; (void)count; (void)nid; return 0; }
int32_t *runtime_errno(void) { static int32_t value; return &value; }
int32_t runtime_guest_errno(int host_errno) { return host_errno; }
void runtime_guest_backtrace(char *out, size_t size, int max) { (void)max; if (size) out[0]=0; }
uint64_t runtime_wait_begin(void) { return 0; }
void runtime_wait_end(const char *what, uint64_t start, const char *detail, long long arg) { (void)what; (void)start; (void)detail; (void)arg; }

int main(void) {
    char out[1100];
    assert(runtime_file_mount("/app0","game")==0);
    assert(translate("/app0/Media/data.unity3d",out,sizeof(out))==0 && !strcmp(out,"game/Media/data.unity3d"));
    assert(translate("Media/x",out,sizeof(out))==0 && !strcmp(out,"game/Media/x"));
    /* Names that only contain dots are files, not parent folders. */
    assert(translate("/app0/a..b/..c/d..",out,sizeof(out))==0 && !strcmp(out,"game/a..b/..c/d.."));
    assert(translate("/app0/../secret",out,sizeof(out))==EACCES);
    assert(translate("/app0/a/..",out,sizeof(out))==EACCES);
    assert(translate("/app0/..\\..\\secret",out,sizeof(out))==EACCES);
    assert(translate("/app0/a\\..\\..\\secret",out,sizeof(out))==EACCES);
    assert(translate("/app0/a\\..",out,sizeof(out))==EACCES);
    assert(translate("..\\secret",out,sizeof(out))==EACCES);
    assert(translate("/nowhere/x",out,sizeof(out))==ENOENT);
    puts("file paths test: ok");
    return 0;
}
