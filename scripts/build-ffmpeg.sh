#!/usr/bin/env bash
# Cross-compiles a minimal FFmpeg (H.264 + AAC decoders, MPEG-TS/MP4 demuxers) for the 3DS.
# Run inside the devkitpro/devkitarm image:
#   docker run --rm -v "$PWD":/src -w /src devkitpro/devkitarm scripts/build-ffmpeg.sh
set -euo pipefail

FFMPEG_VERSION=7.1.2
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/build/ffmpeg-src"
PREFIX="$ROOT/third_party/ffmpeg-3ds"
export PATH="$DEVKITARM/bin:$PATH"

mkdir -p "$WORK"
cd "$WORK"
if [ ! -d "ffmpeg-$FFMPEG_VERSION" ]; then
    curl -fsSL "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz" | tar xJ
fi
cd "ffmpeg-$FFMPEG_VERSION"

ARCH_FLAGS="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft"

./configure \
    --prefix="$PREFIX" \
    --enable-cross-compile --cross-prefix=arm-none-eabi- \
    --arch=arm --cpu=armv6k --target-os=none \
    --extra-cflags="$ARCH_FLAGS -O2 -ffunction-sections -fdata-sections -D__3DS__ -Wno-error=incompatible-pointer-types -Wno-error=int-conversion -Wno-error=implicit-function-declaration -I$DEVKITPRO/libctru/include" \
    --extra-ldflags="$ARCH_FLAGS -specs=3dsx.specs -L$DEVKITPRO/libctru/lib" \
    --extra-libs="-lctru -lm" \
    --disable-everything --disable-programs --disable-doc --disable-debug \
    --disable-avdevice --disable-avfilter --disable-swscale --disable-swresample \
    --disable-network --disable-pthreads --disable-w32threads --disable-os2threads \
    --disable-neon --disable-iconv --disable-zlib --disable-bzlib --disable-lzma \
    --disable-runtime-cpudetect --disable-hwaccels --disable-vaapi --disable-vdpau \
    --enable-static --disable-shared \
    --enable-decoder=h264,aac,aac_latm,mp3,mp3float \
    --enable-parser=h264,aac,aac_latm,mpegaudio \
    --enable-demuxer=mpegts,mov,aac,h264 \
    --enable-bsf=h264_mp4toannexb \
    --enable-protocol=file

make -j"$(nproc)"
make install
echo "FFmpeg installed to $PREFIX"
