#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# The ffmpeg the DLC maker's release ships: built from a pinned FFmpeg release with only what core/media.cpp
# and core/image.cpp ask of it, linked statically (FFmpeg's own libraries, and on Windows libx264, libdav1d
# and zlib too), so the release carries one program instead of a distribution's ffmpeg and its ~110
# libraries.
#
#     bash tools/dlcmaker/build-ffmpeg.sh OUT_DIR     ffmpeg(.exe) into OUT_DIR
#
# Windows: MSYS2 CLANG64 (mingw-w64-clang-x86_64-{toolchain,nasm,x264,dav1d,zlib} and make). Linux: clang,
# nasm, make, libx264-dev, libdav1d-dev, zlib1g-dev (the release bundles libx264 and libdav1d, which
# Ubuntu ships as shared libraries only, beside it).
#
# What it reads: audio (mp3, aac/m4a, ogg vorbis/opus, flac, alac, wav) alone or as a video's track;
# video (mp4/mov, mkv/webm, avi with h264, hevc, vp8, vp9, av1, mpeg4, mjpeg); pictures (png, jpeg, webp).
# What it writes: 48 kHz 16-bit wav (afade), H.264 mp4 (fps, scale, pad, format; or the source copied
# when it already fits) and png (a video frame, a WebP turned into PNG for stb_image).
set -euo pipefail
out=${1:?usage: build-ffmpeg.sh OUT_DIR}
mkdir -p "$out"
out=$(cd "$out" && pwd)
FFMPEG_VERSION=${FFMPEG_VERSION:-9.0.2}
FFMPEG_SHA256=${FFMPEG_SHA256:-8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e}

case $(uname -s) in
Linux) windows=0; cc=${CC:-clang-18}; jobs=$(nproc) ;;
*)
    windows=1
    msys2_root=${MSYS2_ROOT:-/c/msys64}
    export PATH="$msys2_root/clang64/bin:$msys2_root/usr/bin:$PATH"
    cc=clang
    jobs=$(nproc) ;;
esac

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
curl -fsSL -o "$work/ffmpeg.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
echo "$FFMPEG_SHA256  $work/ffmpeg.tar.xz" | sha256sum -c - >/dev/null
tar xJf "$work/ffmpeg.tar.xz" -C "$work"

list() { local IFS=,; echo "$*"; }
demuxers=(mov matroska avi mp3 ogg flac wav aac image2 png_pipe jpeg_pipe webp_pipe)
decoders=(mp3 mp3float aac aac_fixed flac vorbis opus alac pcm_s16le pcm_s16be pcm_s24le pcm_s32le pcm_f32le pcm_u8
          h264 hevc vp8 vp9 libdav1d mpeg4 mjpeg png webp)
encoders=(libx264 pcm_s16le png)
muxers=(mp4 wav image2 image2pipe)
parsers=(h264 hevc aac mpegaudio vorbis opus flac vp8 vp9 av1 mpeg4video mjpeg png webp)
bsfs=(aac_adtstoasc h264_mp4toannexb hevc_mp4toannexb extract_extradata vp9_superframe_split)
# What the command lines name, and what ffmpeg puts in by itself (sample and pixel conversion, a phone
# video's rotation, the graph's ends).
filters=(afade fps scale pad format aformat aresample anull null copy trim atrim setpts asetpts transpose hflip vflip
         buffer buffersink abuffer abuffersink)

flags=(--disable-everything --disable-autodetect --disable-network --disable-doc --disable-debug
       --disable-ffplay --disable-ffprobe --disable-avdevice --enable-gpl --enable-static --disable-shared
       --enable-zlib --enable-libx264 --enable-libdav1d --enable-swscale --enable-swresample
       --enable-protocol=file,pipe --enable-demuxer="$(list "${demuxers[@]}")" --enable-decoder="$(list "${decoders[@]}")"
       --enable-encoder="$(list "${encoders[@]}")" --enable-muxer="$(list "${muxers[@]}")"
       --enable-parser="$(list "${parsers[@]}")" --enable-bsf="$(list "${bsfs[@]}")" --enable-filter="$(list "${filters[@]}")"
       --cc="$cc")
if ((windows)); then
    flags+=(--target-os=mingw32 --arch=x86_64 --enable-w32threads --pkg-config-flags=--static --extra-ldflags=-static)
else
    flags+=(--enable-pthreads)
fi
cd "$work/ffmpeg-$FFMPEG_VERSION"
./configure "${flags[@]}" >"$work/configure.log" 2>&1 || { tail -30 "$work/configure.log" ffbuild/config.log 2>/dev/null; exit 1; }
exe=ffmpeg$( ((windows)) && echo .exe || true)
make -j"$jobs" "$exe" >"$work/make.log" 2>&1 || { tail -40 "$work/make.log"; exit 1; }
cp "$exe" "$out/$exe"
echo "Built $out/$exe (FFmpeg $FFMPEG_VERSION)"
