#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds the DLC maker: app/out/theatrhythm-dlcmaker.exe on Windows (Git Bash, MSYS2 CLANG64 like the
# runner's build.sh; linked statically, so it starts without the MSYS2 DLLs), or app/out/theatrhythm-dlcmaker
# on Linux (clang, SDL3 from pkg-config; Dear ImGui and its font from the runner's gpu/third_party). One
# binary is the window and the command line: core/ (projects, Unity bundles, textures, the game's
# catalog), with stb, etcpak's BC7/BC1 encoders and bcdec from third_party/ and libvorbis/libogg linked
# in. ffmpeg stays a program of its own (THEATRHYTHM_FFMPEG, next to the exe, or PATH).
set -euo pipefail
cd -- "$(dirname -- "$0")"
mkdir -p out out/core
port=../../..
cxx=${CXX:-clang++}
[[ $(uname -s) == Linux ]] || { msys2_root=${MSYS2_ROOT:-/c/msys64}; [[ -d $msys2_root/clang64/bin ]] && export PATH="$msys2_root/clang64/bin:$PATH"; }

# The core and the vendored encoders once into out/core (rebuilt when a source or header is newer).
core_objs=()
headers=(../core/*.h ../core/*.inc)
for src in ../core/*.cpp $port/third_party/etcpak/{bc7enc,ProcessDxtc,Dither}.cpp; do
    obj=out/core/$(basename "$src" .cpp).o
    stale=0
    [[ $obj -nt $src ]] || stale=1
    for h in "${headers[@]}"; do [[ $obj -nt $h ]] || stale=1; done
    if (( stale )); then
        case $src in
        ../core/*) flags=(-Wall -Wextra -Werror) ;;
        *) flags=() ;; # vendored as is
        esac
        "$cxx" -std=c++20 -O2 "${flags[@]}" -I../core -I"$port/third_party/stb" -I"$port/third_party/etcpak" -c "$src" -o "$obj"
    fi
    core_objs+=("$obj")
done

case $(uname -s) in
Linux)
    imgui=$port/gpu/third_party/imgui
    if [[ ! -f $imgui/imgui.h ]]; then
        git -C "$port" submodule update --init gpu/third_party/imgui
    fi
    sdl=$(pkg-config --cflags --libs sdl3) || { echo 'Need SDL3 (docs/building.md).' >&2; exit 1; }
    # ImGui once per build into out/imgui (not with -Werror), then the window.
    mkdir -p out/imgui
    objs=()
    for src in imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp backends/imgui_impl_sdl3.cpp \
               backends/imgui_impl_sdlrenderer3.cpp misc/cpp/imgui_stdlib.cpp; do
        obj=out/imgui/$(basename "$src" .cpp).o
        if [[ ! $obj -nt $imgui/$src ]]; then
            # shellcheck disable=SC2086
            "$cxx" -std=c++20 -O2 -c "$imgui/$src" -I"$imgui" $(pkg-config --cflags sdl3) -o "$obj"
        fi
        objs+=("$obj")
    done
    # shellcheck disable=SC2086
    "$cxx" -std=c++20 -O2 -g -Wall -Wextra -Werror -I"$imgui" -I"$imgui/backends" -I"$port/third_party/stb" \
        -DTHEATRHYTHM_FONT_PATH="\"$(cd "$port/gpu/third_party/fonts" && pwd)/DejaVuSans.ttf\"" \
        main_linux.cpp "${objs[@]}" "${core_objs[@]}" $sdl -Wl,-Bstatic -lvorbis -logg -Wl,-Bdynamic -pthread \
        -o out/theatrhythm-dlcmaker
    echo "Built $PWD/out/theatrhythm-dlcmaker" ;;
*)
    windres app.rc -O coff -o out/app.res.o
    clang++ -std=c++20 -O2 -Wall -Wextra -Werror -DUNICODE -D_UNICODE -municode -mwindows -static \
        main.cpp out/app.res.o "${core_objs[@]}" -lvorbis -logg -lcomctl32 -lole32 -lshell32 -luuid -lwindowscodecs -luxtheme -o out/theatrhythm-dlcmaker.exe
    echo "Built $PWD/out/theatrhythm-dlcmaker.exe" ;;
esac
