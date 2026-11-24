/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The prepared stamp (src/prepared_stamp.h): game/release.txt names the release and the dump's
 * eboot.bin and game code; the image counts as prepared only while all three are unchanged, and an
 * image stamped by an older setup (the release line alone) is prepared again. */
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#include "../src/prepared_stamp.h"
#include <assert.h>
#include <utime.h>
#ifdef _WIN32
#include <direct.h>
#define make_dir(path) _mkdir(path)
#else
#define make_dir(path) mkdir(path, 0755)
#endif

#define ROOT "out" THEATRHYTHM_STAMP_SEP "prepared-stamp-files" /* not the test exe's name (Linux: no .exe) */
#define GAME ROOT THEATRHYTHM_STAMP_SEP "dump"
#define S THEATRHYTHM_STAMP_SEP

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    assert(f);
    fputs(text, f);
    fclose(f);
}

/* Sets a file's last write time, so a change shows without waiting for the clock. */
static void touch(const char *path, long seconds) {
    struct utimbuf t = {seconds, seconds};
    assert(!utime(path, &t));
}

/* Stamps the image as the setup does after preparing it. */
static void stamp(void) {
    char text[THEATRHYTHM_STAMP_CHARS];
    assert(theatrhythm_prepared_stamp(ROOT, GAME, text, sizeof text));
    write_file(ROOT S "game" S "release.txt", text);
}

int main(void) {
    make_dir("out"); make_dir(ROOT); make_dir(ROOT S "game"); make_dir(GAME);
    make_dir(GAME S "Media"); make_dir(GAME S "Media" S "Modules");
    remove(ROOT S "game" S "release.txt");
    write_file(ROOT S "release.txt", "theatrhythm-fbl-pc-v1.0.5d\r\n");
    write_file(GAME S "eboot.bin", "eboot");
    write_file(GAME S THEATRHYTHM_STAMP_CODE, "code 1.00");
    touch(GAME S "eboot.bin", 1000000000);
    touch(GAME S THEATRHYTHM_STAMP_CODE, 1000000000);

    /* Never prepared, then prepared: the stamp has all three lines, the release without its CR. */
    assert(!theatrhythm_prepared(ROOT, GAME));
    stamp();
    assert(theatrhythm_prepared(ROOT, GAME));
    char text[THEATRHYTHM_STAMP_CHARS];
    assert(theatrhythm_prepared_stamp(ROOT, GAME, text, sizeof text));
    assert(!strncmp(text, "release=theatrhythm-fbl-pc-v1.0.5d\ndump=5 ", 42));
    assert(strstr(text, "\ncode=9 ") && text[strlen(text) - 1] == '\n');

    /* Same release unpacked again (release.txt rewritten, same first line): still prepared. */
    write_file(ROOT S "release.txt", "theatrhythm-fbl-pc-v1.0.5d\n");
    assert(theatrhythm_prepared(ROOT, GAME));
    /* A newer release: prepared again. */
    write_file(ROOT S "release.txt", "theatrhythm-fbl-pc-v1.0.6\n");
    assert(!theatrhythm_prepared(ROOT, GAME));
    stamp();
    assert(theatrhythm_prepared(ROOT, GAME));

    /* Another eboot.bin (same size, other time): prepared again. */
    touch(GAME S "eboot.bin", 1000000100);
    assert(!theatrhythm_prepared(ROOT, GAME));
    stamp();
    assert(theatrhythm_prepared(ROOT, GAME));

    /* Other game code (the 1.04 update's prx, same time): prepared again. */
    write_file(GAME S THEATRHYTHM_STAMP_CODE, "code 1.04 longer");
    touch(GAME S THEATRHYTHM_STAMP_CODE, 1000000000);
    assert(!theatrhythm_prepared(ROOT, GAME));
    stamp();
    assert(theatrhythm_prepared(ROOT, GAME));

    /* An image of an older setup: its stamp is release.txt itself. */
    write_file(ROOT S "game" S "release.txt", "theatrhythm-fbl-pc-v1.0.6\n");
    assert(!theatrhythm_prepared(ROOT, GAME));

    /* No game code or no release.txt: no stamp, never prepared. */
    stamp();
    remove(GAME S THEATRHYTHM_STAMP_CODE);
    assert(!theatrhythm_prepared_stamp(ROOT, GAME, text, sizeof text));
    assert(!theatrhythm_prepared(ROOT, GAME));
    write_file(GAME S THEATRHYTHM_STAMP_CODE, "code 1.04 longer");
    remove(ROOT S "release.txt");
    assert(!theatrhythm_prepared(ROOT, GAME));
    puts("prepared stamp: ok");
    return 0;
}
