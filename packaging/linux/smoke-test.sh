#!/usr/bin/env bash
# Checks an unpacked Linux release (tools/package.py) on a distro it was not built on. Run as root in
# a fresh container of that distro (.github/workflows/build.yml): it installs what a desktop already
# has (python3, the Vulkan loader, the X11/Wayland/audio libraries the release leaves to the host),
# then checks without a game dump or a GPU:
#   - the programs find every library (ldd) and start: a missing one or a too-new glibc stops them;
#   - the game, never set up, says so and starts the setup; theatrhythm-setup rejects a folder that
#     is not a dump, and with one that looks like a dump runs prepare/run.py through the system
#     python3 to its end;
#   - a set-up folder (paths.ini, game/) starts the game from it, logging to <save>/log, and one
#     prepared by an older setup has the game run theatrhythm-setup --patch;
#   - the DLC maker's AppImage starts, its programs find their libraries, and Check passes on a new
#     project (decoding its audio with the AppImage's ffmpeg).
# Usage: smoke-test.sh <unpacked release folder> <theatrhythm-dlcmaker.AppImage>
set -euo pipefail
release=$(readlink -f -- "$1")
dlcmaker_appimage=$(readlink -f -- "$2")
if command -v apt-get >/dev/null; then
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends python3 libvulkan1 \
        libx11-6 libx11-xcb1 libxcb1 libxcb-render0 libxcb-shm0 libxext6 libxfixes3 libxrender1 libxi6 libxcursor1 libxrandr2 \
        libwayland-client0 libwayland-egl1 libwayland-cursor0 libxcb-shape0 libxcb-xfixes0 libasound2t64 libpulse0 libudev1 \
        libfontconfig1 libfreetype6 zlib1g libdrm2 libgbm1 libegl1 libgl1 libstdc++6 >/dev/null
elif command -v dnf >/dev/null; then
    dnf install -y -q python3 vulkan-loader libX11 libX11-xcb libxcb libXext libXfixes libXrender libXi \
        libXcursor libXrandr libwayland-client libwayland-egl libwayland-cursor alsa-lib pulseaudio-libs \
        systemd-libs fontconfig freetype zlib-ng-compat libdrm mesa-libgbm libglvnd-egl libglvnd-glx \
        libstdc++ >/dev/null
elif command -v pacman >/dev/null; then
    pacman -Sy --noconfirm --needed python vulkan-icd-loader libx11 libxcb libxext libxfixes libxrender \
        libxi libxcursor libxrandr wayland alsa-lib libpulse systemd-libs fontconfig freetype2 zlib \
        libdrm mesa libglvnd gcc-libs >/dev/null
fi
grep -m1 PRETTY_NAME /etc/os-release
ldd --version | sed -n 1p  # head would end ldd with SIGPIPE (pipefail)
fail() { echo "FAIL: $*" >&2; exit 1; }
# Without a display, as in a container: started without a terminal, the game and the setup would
# otherwise show their messages in a box and wait for it to be closed.
unset DISPLAY WAYLAND_DISPLAY
cd "$release"

for exe in theatrhythm-fbl theatrhythm-setup; do
    [[ -x $exe ]] || fail "$exe is missing or not executable"
    if ldd "$exe" | grep 'not found'; then fail "$exe: libraries missing (above)"; fi
done
status=0
./theatrhythm-fbl --cpu-features || status=$?
[[ $status == 0 || $status == 20 ]] || fail "theatrhythm-fbl --cpu-features exited $status"
usage=$(./theatrhythm-setup --help 2>&1 || true)
grep -q 'usage: theatrhythm-setup' <<<"$usage" || fail "theatrhythm-setup --help has no usage: $usage"

work=$(mktemp -d)
# Not set up: no paths.ini. No terminal (as from a file manager): the game says so on stderr and
# hands over to the setup, which has no display here to ask with.
rm -f paths.ini
status=0
./theatrhythm-fbl </dev/null >"$work/unset.log" 2>&1 || status=$?
grep -q 'The game is not set up yet' "$work/unset.log" || { cat "$work/unset.log"; fail 'no "not set up" message'; }
grep -q 'No terminal and no display' "$work/unset.log" || { cat "$work/unset.log"; fail 'the game did not start the setup'; }
[[ $status == 2 ]] || fail "theatrhythm-fbl without paths.ini exited $status"
status=0
./theatrhythm-setup --patch </dev/null >"$work/patch-unset.log" 2>&1 || status=$?
grep -q 'The game is not set up yet' "$work/patch-unset.log" || { cat "$work/patch-unset.log"; fail 'setup --patch without paths.ini'; }
[[ $status == 1 ]] || fail "theatrhythm-setup --patch without paths.ini exited $status"

# A folder that is not a dump, then one that has the files the setup looks for (empty, so the
# preparation itself fails, in python3).
mkdir -p "$work/empty" "$work/dump/sce_sys" "$work/dump/sce_module" "$work/dump/Media/Modules"
status=0
./theatrhythm-setup --game "$work/empty" --save "$work/save" >"$work/setup-empty.log" 2>&1 || status=$?
grep -q "isn't a decrypted Theatrhythm Final Bar Line dump" "$work/setup-empty.log" ||
    { cat "$work/setup-empty.log"; fail 'the setup took a folder without eboot.bin'; }
for f in eboot.bin sce_sys/param.sfo sce_module/libc.prx sce_module/libSceFios2.prx \
         Media/Modules/PS4Util.prx Media/Modules/Il2CppUserAssemblies.prx; do
    : >"$work/dump/$f"
done
status=0
./theatrhythm-setup --game "$work/dump" --save "$work/save" >"$work/setup-dump.log" 2>&1 || status=$?
cat "$work/setup-dump.log"
grep -q 'Preparing the game from' "$work/setup-dump.log" || fail 'the setup did not start preparing'
grep -q 'Preparing the game failed (exit code' "$work/setup-dump.log" || fail 'the preparation did not run to its end'
grep -q '^  ' "$work/setup-dump.log" || fail "no output of prepare/run.py reached the setup's log"
[[ $status == 1 ]] || fail "theatrhythm-setup with an empty dump exited $status"
[[ ! -e paths.ini ]] || fail 'a failed setup wrote paths.ini'

# Set up by hand: the game starts from paths.ini and logs to <save>/log (no terminal). The boot file
# is not a real one, so it stops there. game/release.txt is the stamp src/prepared_stamp.h checks.
mkdir -p game
printf 'not a boot file' >game/boot-linked.bin
: >game/content.bin
printf '[paths]\ngame=%s\nsave=%s\n' "$work/dump" "$work/save" >paths.ini
python3 - "$work/dump" >game/release.txt <<'EOF' || fail 'could not write the stamp'
import os, sys
def stamp(path):
    st = os.stat(path)
    return f'{st.st_size} {st.st_mtime_ns // 10**9}.{st.st_mtime_ns % 10**9:09d}'
release = open('release.txt', encoding='utf-8').readline().rstrip('\r\n')
print(f'release={release}')
print('dump=' + stamp(os.path.join(sys.argv[1], 'eboot.bin')))
print('code=' + stamp(os.path.join(sys.argv[1], 'Media', 'Modules', 'Il2CppUserAssemblies.prx')))
EOF
status=0
./theatrhythm-fbl </dev/null >"$work/launch.out" 2>&1 || status=$?
log=$work/save/log/theatrhythm.log
[[ -f $log ]] || { cat "$work/launch.out"; fail "no $log"; }
cat "$log"
grep -q "Launch: game $work/dump" "$log" || fail 'the game did not start from paths.ini'
(( status != 0 && status < 128 )) || fail "theatrhythm-fbl exited $status on a bad boot file (0 or a signal)"
! grep -q 'Launch: preparing' "$log" "$work/launch.out" || fail 'the game prepared an image whose stamp matches'

# An image of an older setup (game/release.txt is release.txt itself): the game has the setup prepare
# it again (--patch) from paths.ini, without asking. The dump's files are empty, so that fails, the
# setup says why and the game does not start.
cp release.txt game/release.txt
status=0
./theatrhythm-fbl </dev/null >"$work/patch.out" 2>&1 || status=$?
cat "$work/patch.out"
grep -q 'Launch: preparing the game (theatrhythm-setup --patch)' "$work/patch.out" || fail 'the game did not run setup --patch'
grep -q 'Preparing the game again from paths.ini' "$work/patch.out" || fail "setup --patch's lines did not reach the game's output"
grep -q 'Preparing the game failed (exit code' "$work/patch.out" || fail 'setup --patch did not run the preparation'
! grep -q 'Launch: game' "$work/patch.out" || fail 'the game started without a prepared image'
[[ $status == 1 ]] || fail "theatrhythm-fbl with a failed --patch exited $status"
rm -rf game paths.ini

# The DLC maker, its own download: the AppImage starts (--help: no display here, so not its window; and
# no FUSE in a container, so it extracts itself to run), its programs find their libraries, and its
# command line makes a new project which, with a tone as its audio, passes Check: that decodes the tone
# with the AppImage's own ffmpeg (no game dump needed; the container has no ffmpeg of its own).
dlcmaker() { APPIMAGE_EXTRACT_AND_RUN=1 "$dlcmaker_appimage" "$@"; }
usage=$(dlcmaker --help 2>&1 || true)
grep -q 'usage: theatrhythm-dlcmaker' <<<"$usage" || fail "theatrhythm-dlcmaker.AppImage --help has no usage: $usage"
(cd "$work" && "$dlcmaker_appimage" --appimage-extract >/dev/null) || fail 'the AppImage does not extract'
for exe in theatrhythm-dlcmaker ffmpeg; do
    exe=$work/squashfs-root/$exe
    [[ -x $exe ]] || fail "$exe is missing or not executable"
    if ldd "$exe" | grep 'not found'; then fail "$exe: libraries missing (above)"; fi
done
"$work/squashfs-root/ffmpeg" -hide_banner -version | sed -n 1p
dlcmaker new "$work/projects/Smoke Test" || fail 'dlcmaker new failed'
python3 - "$work/projects/Smoke Test/My Song/audio.wav" <<'EOF' || fail 'could not write the test tone'
import math, struct, sys, wave
with wave.open(sys.argv[1], 'wb') as w:  # 60 s of 440 Hz, 44.1 kHz mono: Check resamples it
    w.setnchannels(1), w.setsampwidth(2), w.setframerate(44100)
    w.writeframes(b''.join(struct.pack('<h', int(8000 * math.sin(2 * math.pi * 440 * i / 44100))) for i in range(44100 * 60)))
EOF
status=0
dlcmaker check "$work/projects" >"$work/check.log" 2>&1 || status=$?
cat "$work/check.log"
[[ $status == 0 ]] || fail "dlcmaker check exited $status"
grep -q '^0 error(s)' "$work/check.log" || fail 'dlcmaker check found errors'
echo "PASS: $(grep -m1 PRETTY_NAME /etc/os-release | cut -d= -f2)"
