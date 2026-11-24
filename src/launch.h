/* Double-click start: the command line run.py would pass, built from paths.ini. */
#ifndef THEATRHYTHM_LAUNCH_H
#define THEATRHYTHM_LAUNCH_H

/* theatrhythm-fbl started with no boot file (no arguments, or only options): reads paths.ini next to
 * the exe (theatrhythm-setup writes it; without it the setup is started), has the setup prepare
 * game/ again (--patch) when it was not prepared for this release and dump (src/prepared_stamp.h)
 * and replaces *argc and *argv with
 * <exe> game/boot-linked.bin --content-profile game/content.bin --app0 <game> --user <save>
 * --timeout 0 [--isa-rewrite game/isa-rewrite.bin] and then the options given. Started without a
 * console or terminal (Explorer, a file manager, a shortcut), output goes to
 * <save>/log/theatrhythm.log; --console keeps it on (Windows: opens) a console instead. 0 on
 * success; otherwise it has told the user why. */
int launch_from_paths_ini(int *argc, char ***argv);

#endif
