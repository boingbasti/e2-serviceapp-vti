#!/bin/bash
# Baut FFmpeg 6.1.1 für VTi MIPS (mipsel, VU+ Solo2)
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
    patch -p1 -d "ffmpeg-${FFMPEG_VER}" < "${BUILDDIR}/../../exteplayer3-build/ffmpeg-hls-native-preselect.patch"
fi

cd "ffmpeg-${FFMPEG_VER}"

echo "=== Kompiliere glibc-Compat-Stubs ==="
COMPAT_OBJ="${BUILDDIR}/glibc_compat_ffmpeg.o"
mipsel-linux-gnu-gcc -O2 -mips32 -mhard-float \
    -U__USE_TIME_BITS64 -D_TIME_BITS=32 \
    -c "${BUILDDIR}/glibc_compat_ffmpeg.c" -o "${COMPAT_OBJ}"

echo "=== Konfiguriere FFmpeg ==="
# libxml2 (fuer den DASH-Demuxer): Header stammen aus dem eigenen Cross-Build
# (2.12.9, ohne Module/Threads/HTTP-Client, architekturunabhaengig und 1:1
# vom ARM-Sysroot uebernommen), das eigentliche Linker-Ziel ist aber bewusst
# die reale, bereits im VTi-Feed installierte libxml2.so.2.9.2
# (mips/sysroot/usr/lib/), damit zur Laufzeit auf der Box exakt dieselbe
# Bibliothek verwendet wird, die schon da ist - libxml2 wird deshalb NICHT
# mit ausgeliefert, sondern als opkg-Depends im ffmpeg-Paket eingetragen
# (siehe package-ffmpeg-ipk.sh), analog zu ARM.
export PKG_CONFIG_LIBDIR="${SYSROOT}/usr/lib/pkgconfig"
export PKG_CONFIG_PATH=""

./configure \
  --pkg-config=pkg-config \
  --prefix="${PREFIX}" \
  --enable-shared \
  --disable-static \
  --enable-cross-compile \
  --cross-prefix=mipsel-linux-gnu- \
  --arch=mips \
  --cpu=mips32 \
  --target-os=linux \
  --sysroot="${SYSROOT}" \
  --enable-gpl \
  --enable-nonfree \
  --enable-openssl \
  --disable-mipsfpu \
  --disable-mipsdsp \
  --disable-mipsdspr2 \
  --disable-mips32r2 \
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
  --extra-cflags="-O2 -mips32 -mhard-float -U__USE_TIME_BITS64 -D_TIME_BITS=32 -I${SYSROOT}/usr/include" \
  --extra-ldflags="-L${SYSROOT}/usr/lib -Wl,-rpath,/usr/lib/exteplayer3_deps -Wl,--hash-style=sysv ${COMPAT_OBJ}" \
  --extra-libs="-lssl -lcrypto -ldl -lpthread -lm"

echo "=== Kompiliere FFmpeg ($(nproc) Kerne) ==="
make -j$(nproc)
make install

echo "=== Kopiere kompilierte Dateien ==="

rm -rf "${BUILDDIR}/ffmpeg-libs"
mkdir -p "${BUILDDIR}/ffmpeg-libs"
cp -d "${PREFIX}/lib"/lib*.so* "${BUILDDIR}/ffmpeg-libs/"
cp -d "${SYSROOT}/usr/lib"/libz.so* "${BUILDDIR}/ffmpeg-libs/"
echo "Shared-Libs: $(ls ${BUILDDIR}/ffmpeg-libs/*.so | wc -l) Dateien"
for lib in "${BUILDDIR}/ffmpeg-libs"/lib*.so*; do
    if [ -f "$lib" ] && [ ! -L "$lib" ]; then
        python3 "${BUILDDIR}/patch_glibc_version.py" "$lib"
    fi
done

mkdir -p "${BUILDDIR}/ffmpeg-bin"
for bin in ffmpeg ffprobe; do
    if [ -f "${PREFIX}/bin/${bin}" ]; then
        mipsel-linux-gnu-strip "${PREFIX}/bin/${bin}"
        python3 "${BUILDDIR}/patch_glibc_version.py" "${PREFIX}/bin/${bin}"
        patchelf --add-needed libpthread.so.0 "${PREFIX}/bin/${bin}"
        python3 -c "with open('${PREFIX}/bin/${bin}', 'r+b') as f: f.seek(8); f.write(b'\x00')"
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
