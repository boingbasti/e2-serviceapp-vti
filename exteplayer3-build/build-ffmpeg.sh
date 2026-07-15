#!/bin/bash
# Baut FFmpeg 6.1.1 für VTi (ARM armv7ahf, VU+ Uno 4K SE)
# Ergebnis: ffmpeg-libs/ (Shared-Libs), ffmpeg-bin/ (ffmpeg + ffprobe)
set -e

BUILDDIR="$(cd "$(dirname "$0")" && pwd)"
SYSROOT="${BUILDDIR}/../sysroot"
SRC_DIR="${BUILDDIR}/ffmpeg-src"
PREFIX="${BUILDDIR}/ffmpeg-build-out"

FFMPEG_VER="6.1.1"
FFMPEG_TAR="ffmpeg-${FFMPEG_VER}.tar.xz"
FFMPEG_URL="https://ffmpeg.org/releases/${FFMPEG_TAR}"

mkdir -p "${SRC_DIR}"
cd "${SRC_DIR}"

if [ ! -f "${FFMPEG_TAR}" ]; then
    echo "=== Downloade FFmpeg ${FFMPEG_VER} ==="
    curl -L -O "${FFMPEG_URL}"
fi

if [ ! -d "ffmpeg-${FFMPEG_VER}" ]; then
    echo "=== Entpacke FFmpeg ==="
    tar xf "${FFMPEG_TAR}"
    echo "=== Appliere HLS Preselect Patch ==="
    patch -p1 -d "ffmpeg-${FFMPEG_VER}" < "${BUILDDIR}/ffmpeg-hls-native-preselect.patch"
fi

cd "ffmpeg-${FFMPEG_VER}"

echo "=== Kompiliere glibc-Compat-Stubs ==="
COMPAT_OBJ="${BUILDDIR}/glibc_compat_ffmpeg.o"
arm-linux-gnueabihf-gcc -O2 -march=armv7-a -mfpu=neon -mfloat-abi=hard \
    -U__USE_TIME_BITS64 -D_TIME_BITS=32 \
    -c "${BUILDDIR}/glibc_compat_ffmpeg.c" -o "${COMPAT_OBJ}"

echo "=== Konfiguriere FFmpeg ==="
./configure \
  --prefix="${PREFIX}" \
  --enable-shared \
  --disable-static \
  --enable-cross-compile \
  --cross-prefix=arm-linux-gnueabihf- \
  --arch=arm \
  --cpu=cortex-a15 \
  --target-os=linux \
  --sysroot="${SYSROOT}" \
  --enable-gpl \
  --enable-nonfree \
  --enable-openssl \
  --enable-neon \
  --enable-vfp \
  --enable-armv6 \
  --enable-armv6t2 \
  --disable-armv5te \
  --enable-zlib \
  --disable-doc \
  --disable-debug \
  --disable-htmlpages \
  --disable-manpages \
  --disable-podpages \
  --disable-txtpages \
  --enable-ffmpeg \
  --enable-ffprobe \
  --disable-ffplay \
  --disable-avdevice \
  --enable-postproc \
  --extra-cflags="-O2 -march=armv7-a -mfpu=neon -mfloat-abi=hard -U__USE_TIME_BITS64 -D_TIME_BITS=32 -I${SYSROOT}/usr/include" \
  --extra-ldflags="-L${SYSROOT}/usr/lib/arm -L${SYSROOT}/usr/lib -Wl,-rpath,/usr/lib/exteplayer3_deps ${COMPAT_OBJ}" \
  --extra-libs="-lssl -lcrypto -ldl -lpthread"

echo "=== Kompiliere FFmpeg ($(nproc) Kerne) ==="
make -j$(nproc)
make install

echo "=== Kopiere kompilierte Dateien ==="

rm -rf "${BUILDDIR}/ffmpeg-libs"
mkdir -p "${BUILDDIR}/ffmpeg-libs"
cp -d "${PREFIX}/lib"/lib*.so* "${BUILDDIR}/ffmpeg-libs/"
cp -d "${SYSROOT}/usr/lib/arm"/libz.so* "${BUILDDIR}/ffmpeg-libs/"
for libfile in "${BUILDDIR}/ffmpeg-libs"/lib*.so*; do
    if [ -f "$libfile" ] && [ ! -L "$libfile" ]; then
        python3 "${BUILDDIR}/patch_glibc_version.py" "$libfile"
    fi
done
echo "Shared-Libs: $(ls ${BUILDDIR}/ffmpeg-libs/*.so | wc -l) Dateien"

mkdir -p "${BUILDDIR}/ffmpeg-bin"
for bin in ffmpeg ffprobe; do
    if [ -f "${PREFIX}/bin/${bin}" ]; then
        arm-linux-gnueabihf-strip "${PREFIX}/bin/${bin}"
        python3 "${BUILDDIR}/patch_glibc_version.py" "${PREFIX}/bin/${bin}"
        patchelf --add-needed libpthread.so.0 "${PREFIX}/bin/${bin}"
        cp "${PREFIX}/bin/${bin}" "${BUILDDIR}/ffmpeg-bin/"
        echo "${bin}: $(ls -lh ${BUILDDIR}/ffmpeg-bin/${bin})"
    fi
done

mkdir -p "${BUILDDIR}/ffmpeg-6.1.1"
cp -r "${PREFIX}/include"/* "${BUILDDIR}/ffmpeg-6.1.1/"

echo ""
echo "=== FFmpeg-Build erfolgreich abgeschlossen! ==="
echo "Libs:    ${BUILDDIR}/ffmpeg-libs/"
echo "Binaries: ${BUILDDIR}/ffmpeg-bin/"
