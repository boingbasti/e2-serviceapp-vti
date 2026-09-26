#!/bin/bash
# Baut exteplayer3 für VTi16 (armv7ahf, Duo 4K Lite/SE), verlinkt gegen das
# selbst gebaute FFmpeg 7.0.2.
# Ergebnis: release/exteplayer3_vti_1_182+vti001_armv7ahf-vti16.ipk
#
# Unterschiede zum VTi15-Pendant:
# - Kein glibc_compat.c mehr (nicht noetig, VTi16s glibc ist neu genug).
# - box-sysroot/ komplett frisch von der echten VTi16-Box befuellt.
# - FFMPEG_HEADERS/FFMPEG_LIBS zeigen auf die vti16-eigenen FFmpeg-7.0.2-
#   Build-Ergebnisse (siehe build-ffmpeg.sh in diesem Ordner).
# - Eigener Architektur-Suffix (armv7ahf-vti16) im Paketnamen und im
#   control-Template, damit VTi15/VTi16-Pakete nie kollidieren koennen.
set -e

BUILDDIR="$(cd "$(dirname "$0")" && pwd)"
SRCDIR="${BUILDDIR}/../../exteplayer3-build/src"
FFMPEG_HEADERS="${BUILDDIR}/ffmpeg-7.0.2"
FFMPEG_LIBS="${BUILDDIR}/ffmpeg-libs"
BOXSYSROOT="${BUILDDIR}/box-sysroot"
CC=arm-linux-gnueabihf-gcc

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
"

echo "=== Compiling exteplayer3 fuer VTi16 ==="
$CC $SOURCE_FILES \
  -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE -D_LARGEFILE_SOURCE \
  -D_TIME_BITS=64 \
  -DHAVE_FLV2MPEG4_CONVERTER \
  -I$SRCDIR/include \
  -I$SRCDIR/external \
  -I$SRCDIR/external/flv2mpeg4 \
  -I$SRCDIR/external/plugins \
  -I$FFMPEG_HEADERS \
  -L$BOXSYSROOT/lib \
  -L$FFMPEG_LIBS \
  "${BOXSYSROOT}/lib/ld-linux-armhf.so.3" \
  -lpthread -ldl -lavformat -lavcodec -lavutil -lswresample -lswscale \
  -Wl,-rpath,/usr/lib/exteplayer3_deps \
  -Wl,--allow-shlib-undefined \
  -o $BUILDDIR/exteplayer3_new

arm-linux-gnueabihf-strip $BUILDDIR/exteplayer3_new
echo "Binary: $(ls -lh $BUILDDIR/exteplayer3_new)"

echo "=== Packaging IPK ==="
rm -rf repack
mkdir -p repack/data_root/usr/bin
mkdir -p repack/ctrl_root
# Architecture: muss ein von VTi16s opkg (arch.conf) anerkanntes Tag sein
# (armv7ahf-neon) - "armv7ahf-vti16" wuerde vom Solver als "does not have a
# compatible architecture" abgelehnt. Der Dateiname traegt "-vti16" trotzdem
# rein zur eigenen Unterscheidung von den VTi15-Paketen (install.sh waehlt
# per Dateiname aus, nicht per Architecture:-Feld).
echo -e "Package: exteplayer3\nVersion: 1:182+vti001\nDescription: exteplayer3 - media player for E2 (VTi16-Build), built from skyjet18/exteplayer3 master (Version 182). Includes FFmpeg 7.0.2 shared libraries in /usr/lib/exteplayer3_deps/.\nSection: libs\nPriority: optional\nMaintainer: saufsoldat\nLicense: GPL-2.0\nHomepage: https://github.com/boingbasti/e2-serviceapp-vti\nArchitecture: armv7ahf-neon\nOE: exteplayer3\nDepends: libc6 (>= 2.40), libxml2\nSource: git://github.com/skyjet18/exteplayer3.git;branch=master;protocol=https" > repack/ctrl_root/control
echo "2.0" > repack/debian-binary
cp $BUILDDIR/exteplayer3_new repack/data_root/usr/bin/exteplayer3
chmod 755 repack/data_root/usr/bin/exteplayer3
mkdir -p repack/data_root/usr/lib/exteplayer3_deps
cp -d $FFMPEG_LIBS/lib*.so* repack/data_root/usr/lib/exteplayer3_deps/
rm -f repack/data_root/usr/lib/exteplayer3_deps/libz.so*
tar czf repack/data.tar.gz -C repack/data_root --owner=0 --group=0 ./usr
tar czf repack/control.tar.gz -C repack/ctrl_root --owner=0 --group=0 ./control
mkdir -p "${BUILDDIR}/../../release"
ar rcs "${BUILDDIR}/../../release/exteplayer3_vti_1_182+vti001_armv7ahf-vti16.ipk" repack/debian-binary repack/data.tar.gz repack/control.tar.gz
echo "IPK: $(ls -lh "${BUILDDIR}/../../release/exteplayer3_vti_1_182+vti001_armv7ahf-vti16.ipk")"
