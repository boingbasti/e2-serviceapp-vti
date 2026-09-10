#!/bin/bash
# Baut exteplayer3 für MIPS (mipsel, VU+ Solo2)
# Ergebnis: release/exteplayer3_vti_1_181+git3_mips32el.ipk
set -e

BUILDDIR="$(cd "$(dirname "$0")" && pwd)"
SRCDIR="${BUILDDIR}/../../exteplayer3-build/src"
FFMPEG_HEADERS="${BUILDDIR}/ffmpeg-6.1.1"
FFMPEG_LIBS="${BUILDDIR}/ffmpeg-libs"
SYSROOT="${BUILDDIR}/../sysroot"

CC="mipsel-linux-gnu-gcc"

SOURCE_FILES="
  $SRCDIR/main/exteplayer.c
  $SRCDIR/tools/debug.c
  $SRCDIR/tools/strbuffer.c
  $SRCDIR/container/container.c
  $SRCDIR/container/container_ffmpeg.c
  $SRCDIR/manager/manager.c
  $SRCDIR/manager/audio.c
  $SRCDIR/manager/video.c
  $SRCDIR/manager/subtitle.c
  $SRCDIR/output/output_subtitle.c
  $SRCDIR/output/graphic_subtitle.c
  $SRCDIR/output/output.c
  $SRCDIR/output/writer/common/pes.c
  $SRCDIR/output/writer/common/misc.c
  $SRCDIR/output/writer/common/writer.c
  $SRCDIR/output/linuxdvb_buffering.c
  $SRCDIR/playback/playback.c
  $SRCDIR/external/ffmpeg/src/bitstream.c
  $SRCDIR/external/ffmpeg/src/latmenc.c
  $SRCDIR/external/ffmpeg/src/mpeg4audio.c
  $SRCDIR/external/flv2mpeg4/src/m4vencode.c
  $SRCDIR/external/flv2mpeg4/src/flvdecoder.c
  $SRCDIR/external/flv2mpeg4/src/dcprediction.c
  $SRCDIR/external/flv2mpeg4/src/flv2mpeg4.c
  $SRCDIR/external/plugins/src/png.c
  $SRCDIR/output/linuxdvb_mipsel.c
  $SRCDIR/output/writer/mipsel/writer.c
  $SRCDIR/output/writer/mipsel/aac.c
  $SRCDIR/output/writer/mipsel/ac3.c
  $SRCDIR/output/writer/mipsel/mp3.c
  $SRCDIR/output/writer/mipsel/pcm.c
  $SRCDIR/output/writer/mipsel/lpcm.c
  $SRCDIR/output/writer/mipsel/dts.c
  $SRCDIR/output/writer/mipsel/amr.c
  $SRCDIR/output/writer/mipsel/bcma.c
  $SRCDIR/output/writer/mipsel/h265.c
  $SRCDIR/output/writer/mipsel/h264.c
  $SRCDIR/output/writer/mipsel/mpeg2.c
  $SRCDIR/output/writer/mipsel/mpeg4.c
  $SRCDIR/output/writer/mipsel/divx3.c
  $SRCDIR/output/writer/mipsel/vp.c
  $SRCDIR/output/writer/mipsel/wmv.c
  $SRCDIR/output/writer/mipsel/vc1.c
  $SRCDIR/output/writer/mipsel/mjpeg.c
  $BUILDDIR/../glibc_compat.c
"

echo "=== Kompiliere exteplayer3 für MIPS ==="
$CC $SOURCE_FILES \
  -O2 \
  -mips32 \
  -mhard-float \
  -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE -D_LARGEFILE_SOURCE \
  -DHAVE_FLV2MPEG4_CONVERTER \
  -include$BUILDDIR/../glibc_version_pin.h \
  -I$SRCDIR/include \
  -I$SRCDIR/external \
  -I$SRCDIR/external/flv2mpeg4 \
  -I$SRCDIR/external/plugins \
  -I$FFMPEG_HEADERS \
  -I$SYSROOT/usr/include \
  -L$SYSROOT/usr/lib \
  -L$FFMPEG_LIBS \
  -Wl,--no-as-needed $SYSROOT/usr/lib/libpthread.so.0 -Wl,--as-needed -ldl -lavformat -lavcodec -lavutil -lswresample -lswscale \
  -Wl,-rpath,/usr/lib/exteplayer3_deps \
  -Wl,--allow-shlib-undefined \
  -Wl,--hash-style=sysv \
  -o $BUILDDIR/exteplayer3_new

mipsel-linux-gnu-strip $BUILDDIR/exteplayer3_new
python3 $BUILDDIR/patch_glibc_version.py $BUILDDIR/exteplayer3_new
echo "Binary: $(ls -lh $BUILDDIR/exteplayer3_new)"

echo "=== Verpacke IPK ==="
rm -rf "${BUILDDIR}/repack"
mkdir -p "${BUILDDIR}/repack/data_root/usr/bin"
mkdir -p "${BUILDDIR}/repack/data_root/usr/lib/exteplayer3_deps"
mkdir -p "${BUILDDIR}/repack/ctrl_root"

# Kopiere Binaries
cp $BUILDDIR/exteplayer3_new "${BUILDDIR}/repack/data_root/usr/bin/exteplayer3"
python3 -c "with open('${BUILDDIR}/repack/data_root/usr/bin/exteplayer3', 'r+b') as f: f.seek(8); f.write(b'\x00')"
chmod 755 "${BUILDDIR}/repack/data_root/usr/bin/exteplayer3"

# Kopiere MIPS FFmpeg Bibliotheken
cp -d "${FFMPEG_LIBS}"/lib*.so* "${BUILDDIR}/repack/data_root/usr/lib/exteplayer3_deps/"
rm -f "${BUILDDIR}/repack/data_root/usr/lib/exteplayer3_deps"/libz.so*

# libavformat (MIPS) braucht libatomic.so.1 zur Laufzeit (mips32el hat keine
# nativen Atomic-Instruktionen). Kommt ueber die Depends-Zeile unten aus dem
# VTi-Feed-Paket "libatomic1" (gehoert zum gcc-runtime, ist dort vorhanden),
# wird NICHT mitgepackt - fuer Shared Libs ist genau dafuer der Feed da.
for lib in "${BUILDDIR}/repack/data_root/usr/lib/exteplayer3_deps"/lib*.so*; do
    if [ -f "$lib" ] && [ ! -L "$lib" ]; then
        python3 -c "with open('$lib', 'r+b') as f: f.seek(8); f.write(b'\x00')"
    fi
done

# Kopiere MIPS systemabhängige Symlinks falls nötig, bzw. bereinige Dateirechte
chmod 755 "${BUILDDIR}/repack/data_root/usr/lib/exteplayer3_deps"/lib*.so*

# Erstelle control File
cat > "${BUILDDIR}/repack/ctrl_root/control" << EOF
Package: exteplayer3
Version: 1:181+git3
Description: exteplayer3 - media player for E2, built from skyjet18/exteplayer3 master with 3 additional post-181 commits. Includes FFmpeg 6.1.1 shared libraries in /usr/lib/exteplayer3_deps/.
Section: libs
Priority: optional
Maintainer: saufsoldat
License: GPL-2.0
Homepage: https://github.com/boingbasti/e2-serviceapp-vti
Architecture: mips32el
OE: exteplayer3
Depends: libc6 (>= 2.20), libatomic1, libxml2
Source: git://github.com/skyjet18/exteplayer3.git;branch=master;protocol=https
EOF

echo "2.0" > "${BUILDDIR}/repack/debian-binary"

tar czf "${BUILDDIR}/repack/data.tar.gz" -C "${BUILDDIR}/repack/data_root" --owner=0 --group=0 ./usr
tar czf "${BUILDDIR}/repack/control.tar.gz" -C "${BUILDDIR}/repack/ctrl_root" --owner=0 --group=0 ./control

mkdir -p "${BUILDDIR}/../../release"
ar rcs "${BUILDDIR}/../../release/exteplayer3_vti_1_181+git3_mips32el.ipk" \
  "${BUILDDIR}/repack/debian-binary" \
  "${BUILDDIR}/repack/data.tar.gz" \
  "${BUILDDIR}/repack/control.tar.gz"

echo "IPK: $(ls -lh "${BUILDDIR}/../../release/exteplayer3_vti_1_181+git3_mips32el.ipk")"
rm -rf "${BUILDDIR}/repack"
