#!/bin/bash
# Baut FFmpeg 7.0.2 für VTi16 (ARM armv7ahf, Duo 4K Lite/SE)
# Ergebnis: ffmpeg-libs/ (Shared-Libs), ffmpeg-bin/ (ffmpeg + ffprobe)
#
# Unterschiede zum VTi15-Pendant (exteplayer3-build/build-ffmpeg.sh im
# Wurzelverzeichnis):
# - FFMPEG_VER=7.0.2 statt 6.1.1.
# - Kein glibc_compat_ffmpeg.o mehr (nicht noetig, VTi16s glibc ist neu genug).
# - -D_TIME_BITS=64 statt "-U__USE_TIME_BITS64 -D_TIME_BITS=32".
# - Kein patch_glibc_version.py-Nachbearbeitungsschritt.
# - HLS-Patch wird per Relativpfad aus dem Wurzel-exteplayer3-build/
#   wiederverwendet (MIPS-Muster), keine eigene Kopie.
set -e

BUILDDIR="$(cd "$(dirname "$0")" && pwd)"
SYSROOT="${BUILDDIR}/../sysroot"
SRC_DIR="${BUILDDIR}/ffmpeg-src"
PREFIX="${BUILDDIR}/ffmpeg-build-out"

FFMPEG_VER="7.0.2"
FFMPEG_TAR="ffmpeg-${FFMPEG_VER}.tar.xz"
FFMPEG_URL="https://ffmpeg.org/releases/${FFMPEG_TAR}"

mkdir -p "${SRC_DIR}"
cd "${SRC_DIR}"

if [ ! -f "${FFMPEG_TAR}" ]; then
    echo "=== Downloade FFmpeg ${FFMPEG_VER} ==="
    curl -L -O "${FFMPEG_URL}"
fi

if [ ! -d "ffmpeg-${FFMPEG_VER}/.patched" ]; then
    if [ ! -d "ffmpeg-${FFMPEG_VER}" ]; then
        echo "=== Entpacke FFmpeg ==="
        tar xf "${FFMPEG_TAR}"
    fi
    echo "=== Appliere HLS Preselect Patch (aus dem Wurzel-exteplayer3-build/) ==="
    patch -p1 -d "ffmpeg-${FFMPEG_VER}" < "${BUILDDIR}/../../exteplayer3-build/ffmpeg-hls-native-preselect.patch" || true
    echo "=== Appliere CENC/DRM-Support-Patch (archivCZSK-Kompatibilitaet, aus dem Wurzel-exteplayer3-build/) ==="
    patch -p1 -d "ffmpeg-${FFMPEG_VER}" < "${BUILDDIR}/../../exteplayer3-build/ffmpeg-cenc-drm-support.patch" || true
    echo "=== Appliere EAC3-Stream-Type-Fix (PMT-Stream-Type 0x87, aus dem Wurzel-exteplayer3-build/) ==="
    patch -p1 -d "ffmpeg-${FFMPEG_VER}" < "${BUILDDIR}/../../exteplayer3-build/ffmpeg-eac3-stream-type-fix.patch" || true
    echo "=== Appliere DASH-AV-Sync-Fix (Live-Streams, getrennte Video/Audio-Nullpunkte, aus dem Wurzel-exteplayer3-build/) ==="
    patch -p1 -d "ffmpeg-${FFMPEG_VER}" < "${BUILDDIR}/../../exteplayer3-build/ffmpeg-dash-avsync-fix.patch" || true
    echo "=== Appliere DASH-Live-Edge-Delay-Fix (Segmentnummer vor Manifest-Veroeffentlichung, aus dem Wurzel-exteplayer3-build/) ==="
    patch -p1 -d "ffmpeg-${FFMPEG_VER}" < "${BUILDDIR}/../../exteplayer3-build/ffmpeg-dash-live-edge-delay-fix.patch" || true
    echo "=== Appliere DASH-Untertitel-Verhungern-Fix (aus dem Wurzel-exteplayer3-build/) ==="
    patch -p1 -d "ffmpeg-${FFMPEG_VER}" < "${BUILDDIR}/../../exteplayer3-build/ffmpeg-dash-subtitle-starvation-fix.patch" || true
    echo "=== Appliere DASH-Kurzpuffer-Startpositions-Fix (aus dem Wurzel-exteplayer3-build/) ==="
    patch -p1 -d "ffmpeg-${FFMPEG_VER}" < "${BUILDDIR}/../../exteplayer3-build/ffmpeg-dash-short-buffer-startup-fix.patch" || true
    touch "ffmpeg-${FFMPEG_VER}/.patched"
fi

cd "ffmpeg-${FFMPEG_VER}"

echo "=== Konfiguriere FFmpeg ==="
export PKG_CONFIG_LIBDIR="${SYSROOT}/usr/lib/arm/pkgconfig"
export PKG_CONFIG_PATH=""

./configure \
  --pkg-config=pkg-config \
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
  --enable-libxml2 \
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
  --extra-cflags="-O2 -march=armv7-a -mfpu=neon -mfloat-abi=hard -D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64 -I${SYSROOT}/usr/include" \
  --extra-ldflags="-L${SYSROOT}/usr/lib/arm -Wl,-rpath,/usr/lib/exteplayer3_deps ${SYSROOT}/usr/lib/arm/ld-linux-armhf.so.3" \
  --extra-libs="-lssl -lcrypto -ldl -lpthread -lm"

echo "=== Kompiliere FFmpeg ($(nproc) Kerne) ==="
make -j$(nproc)
make install

echo "=== Kopiere kompilierte Dateien ==="

rm -rf "${BUILDDIR}/ffmpeg-libs"
mkdir -p "${BUILDDIR}/ffmpeg-libs"
cp -d "${PREFIX}/lib"/lib*.so* "${BUILDDIR}/ffmpeg-libs/"
cp -d "${SYSROOT}/usr/lib/arm"/libz.so* "${BUILDDIR}/ffmpeg-libs/"
echo "Shared-Libs: $(ls ${BUILDDIR}/ffmpeg-libs/*.so | wc -l) Dateien"

mkdir -p "${BUILDDIR}/ffmpeg-bin"
for bin in ffmpeg ffprobe; do
    if [ -f "${PREFIX}/bin/${bin}" ]; then
        arm-linux-gnueabihf-strip "${PREFIX}/bin/${bin}"
        patchelf --add-needed libpthread.so.0 "${PREFIX}/bin/${bin}"
        cp "${PREFIX}/bin/${bin}" "${BUILDDIR}/ffmpeg-bin/"
        echo "${bin}: $(ls -lh ${BUILDDIR}/ffmpeg-bin/${bin})"
    fi
done

rm -rf "${BUILDDIR}/ffmpeg-7.0.2"
mkdir -p "${BUILDDIR}/ffmpeg-7.0.2"
cp -r "${PREFIX}/include"/* "${BUILDDIR}/ffmpeg-7.0.2/"

echo ""
echo "=== FFmpeg-Build erfolgreich abgeschlossen! ==="
echo "Libs:    ${BUILDDIR}/ffmpeg-libs/"
echo "Binaries: ${BUILDDIR}/ffmpeg-bin/"
