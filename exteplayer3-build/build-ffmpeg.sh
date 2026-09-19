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
    echo "=== Appliere CENC/DRM-Support-Patch (archivCZSK-Kompatibilitaet) ==="
    patch -p1 -d "ffmpeg-${FFMPEG_VER}" < "${BUILDDIR}/ffmpeg-cenc-drm-support.patch"
    echo "=== Appliere EAC3-Stream-Type-Fix (PMT-Stream-Type 0x87) ==="
    patch -p1 -d "ffmpeg-${FFMPEG_VER}" < "${BUILDDIR}/ffmpeg-eac3-stream-type-fix.patch"
    echo "=== Appliere DASH-AV-Sync-Fix (Live-Streams, getrennte Video/Audio-Nullpunkte) ==="
    patch -p1 -d "ffmpeg-${FFMPEG_VER}" < "${BUILDDIR}/ffmpeg-dash-avsync-fix.patch"
    echo "=== Appliere DASH-Live-Edge-Delay-Fix (Segmentnummer vor Manifest-Veroeffentlichung) ==="
    patch -p1 -d "ffmpeg-${FFMPEG_VER}" < "${BUILDDIR}/ffmpeg-dash-live-edge-delay-fix.patch"
fi

cd "ffmpeg-${FFMPEG_VER}"

echo "=== Kompiliere glibc-Compat-Stubs ==="
COMPAT_OBJ="${BUILDDIR}/glibc_compat_ffmpeg.o"
arm-linux-gnueabihf-gcc -O2 -march=armv7-a -mfpu=neon -mfloat-abi=hard \
    -U__USE_TIME_BITS64 -D_TIME_BITS=32 \
    -c "${BUILDDIR}/glibc_compat_ffmpeg.c" -o "${COMPAT_OBJ}"

echo "=== Konfiguriere FFmpeg ==="
# libxml2 (fuer den DASH-Demuxer): Header stammen aus einem eigenen Cross-Build
# (2.12.9, ohne Module/Threads/HTTP-Client), das eigentliche Linker-Ziel ist
# aber bewusst die reale, bereits im VTi-Feed installierte libxml2.so.2.9.2
# (sysroot/usr/lib/arm/), damit zur Laufzeit auf der Box exakt dieselbe
# Bibliothek verwendet wird, die schon da ist - libxml2 wird deshalb NICHT
# mit ausgeliefert, sondern als opkg-Depends im ffmpeg-Paket eingetragen
# (siehe package-ffmpeg-ipk.sh), analog zu libatomic1 auf MIPS.
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
  --extra-cflags="-O2 -march=armv7-a -mfpu=neon -mfloat-abi=hard -U__USE_TIME_BITS64 -D_TIME_BITS=32 -I${SYSROOT}/usr/include" \
  --extra-ldflags="-L${SYSROOT}/usr/lib/arm -L${SYSROOT}/usr/lib -Wl,-rpath,/usr/lib/exteplayer3_deps ${COMPAT_OBJ}" \
  --extra-libs="-lssl -lcrypto -ldl -lpthread -lm"

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
