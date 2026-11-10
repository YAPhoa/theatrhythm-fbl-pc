#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds the save editor: app/out/theatrhythm-saveeditor.exe (Git Bash, MSYS2 CLANG64 like the
# runner's build.sh; linked statically, so it starts without the MSYS2 DLLs). One binary is the window
# and the command line. core/ is the save format; names and portraits come from the player's dump
# through the DLC maker's core (tools/dlcmaker/core: its bundles, tables, texts and texture decoder,
# with stb and etcpak from third_party/; stb's zlib also packs the save).
set -euo pipefail
cd -- "$(dirname -- "$0")"
mkdir -p out/core
port=../../..
dlc=../../dlcmaker/core
msys2_root=${MSYS2_ROOT:-/c/msys64}
[[ -d $msys2_root/clang64/bin ]] && export PATH="$msys2_root/clang64/bin:$PATH"

# The save core and the DLC maker's dump readers once into out/core (rebuilt when a source or header
# is newer).
objs=()
headers=(../core/*.h "$dlc"/*.h "$dlc"/*.inc)
for src in ../core/*.cpp "$dlc"/{game,unity,util,value,image,media}.cpp $port/third_party/etcpak/{bc7enc,ProcessDxtc,Dither}.cpp; do
    obj=out/core/$(basename "$src" .cpp).o
    stale=0
    [[ $obj -nt $src ]] || stale=1
    for h in "${headers[@]}"; do [[ $obj -nt $h ]] || stale=1; done
    if (( stale )); then
        case $src in
        */third_party/*) flags=() ;; # vendored as is
        *) flags=(-Wall -Wextra -Werror) ;;
        esac
        clang++ -std=c++20 -O2 "${flags[@]}" -I../core -I"$dlc" -I"$port/third_party/stb" -I"$port/third_party/etcpak" \
            -c "$src" -o "$obj"
    fi
    objs+=("$obj")
done

windres app.rc -O coff -o out/app.res.o
clang++ -std=c++20 -O2 -Wall -Wextra -Werror -DUNICODE -D_UNICODE -municode -mwindows -static -I../core -I"$dlc" \
    main.cpp out/app.res.o "${objs[@]}" -lbcrypt -lcomctl32 -lole32 -lshell32 -luuid -luxtheme \
    -o out/theatrhythm-saveeditor.exe
echo "Built $PWD/out/theatrhythm-saveeditor.exe"
