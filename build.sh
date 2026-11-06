#!/usr/bin/env bash
# Builds out/theatrhythm-fbl and out/theatrhythm-setup (.exe on Windows): MSYS2 CLANG64 there,
# clang and the system libraries on Linux (packages in docs/building.md). The GPU library is static
# and CMake links the probe as well. --test also runs the runtime tests.
set -euo pipefail
cd -- "$(dirname -- "$0")"
mkdir -p out
case $(uname -s) in
MINGW*|MSYS*)
    # shellcheck source=msys2-env.sh
    source ./msys2-env.sh
    exe=.exe
    # The Windows counterparts of the POSIX calls (src/compat_win.h).
    compat=(src/compat_win.c -lbcrypt) net=(-lws2_32) memory=(-lonecore) cflags=() ;;
Linux)
    exe=''
    # -std=c11 hides POSIX from glibc's headers.
    compat=() net=() memory=() cflags=(-D_DEFAULT_SOURCE) ;;
*) echo 'Windows (Git Bash or an MSYS2 shell) or Linux only.' >&2; exit 1 ;;
esac
targets=(theatrhythm-fbl theatrhythm-setup)
cc=${CC:-clang} cxx=${CXX:-clang++}
for tool in "$cc" "$cxx" cmake ninja pkg-config; do
    command -v "$tool" >/dev/null || { echo "Need $tool (docs/building.md)." >&2; exit 1; }
done
if [[ ! -f gpu/third_party/imgui/imgui.h || ! -f gpu/third_party/xbyak/xbyak/xbyak.h ]]; then
    git submodule update --init --recursive
fi
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" >/dev/null
if ! ninja -C out/gpu "${targets[@]}" > out/gpu-build.log 2>&1; then
    grep -v '^\[' out/gpu-build.log | tail -40 >&2
    echo 'Build failed (full log: out/gpu-build.log)' >&2; exit 1
fi
for target in "${targets[@]}"; do cp "out/gpu/$target$exe" out/; done
echo "Built ${targets[*]/%/$exe} in $PWD/out"
if [[ ${1:-} == --test ]]; then
    # run_test <name> <source> [extra flags...]: builds out/<name>-test and runs it.
    run_test() {
        local name=$1 source=$2; shift 2
        "$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${cflags[@]}" -I. -Isrc "$source" "$@" -o "out/$name-test$exe"
        "out/$name-test$exe"
    }
    run_test services tests/test_services.c "${compat[@]}"
    run_test savedata tests/test_savedata.c "${compat[@]}"
    run_test content tests/test_content.c "${compat[@]}"
    run_test eventflag tests/test_eventflag.c
    run_test threadcalls tests/test_threadcalls.c "${compat[@]}"
    run_test posixsem tests/test_posixsem.c
    run_test net tests/test_net.c "${compat[@]}" "${net[@]}"
    run_test kernel-files tests/test_kernel_files.c "${compat[@]}"
    run_test button-icons tests/test_button_icons.c
    run_test memory tests/test_memory.c "${compat[@]}" "${memory[@]}"
    run_test memory-read-backing tests/test_memory_read_backing.c "${compat[@]}" "${memory[@]}"
    ninja -C out/gpu pad-test >/dev/null
    "out/gpu/pad-test$exe"
fi
