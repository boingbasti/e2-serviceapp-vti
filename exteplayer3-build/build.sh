#!/bin/bash
# Baut exteplayer3 für armv7ahf (VU+ Uno 4K SE / VTi enigma2)
# Ergebnis: repack/exteplayer3_vti_182_armv7ahf.ipk
set -e

BUILDDIR="$(cd "$(dirname "$0")" && pwd)"
SRCDIR=$BUILDDIR/src
FFMPEG_HEADERS=$BUILDDIR/ffmpeg-6.1.1
FFMPEG_LIBS=$BUILDDIR/ffmpeg-libs
BOXSYSROOT=$BUILDDIR/box-sysroot
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
  $BUILDDIR/glibc_compat.c
"

echo "=== Compiling exteplayer3 ==="
$CC $SOURCE_FILES \
  -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE -D_LARGEFILE_SOURCE \
  -DHAVE_FLV2MPEG4_CONVERTER \
  -I$SRCDIR/include \
  -I$SRCDIR/external \
  -I$SRCDIR/external/flv2mpeg4 \
  -I$SRCDIR/external/plugins \
  -I$FFMPEG_HEADERS \
  -L$BOXSYSROOT/lib \
  -L$FFMPEG_LIBS \
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
# Setup control template if missing
if [ ! -f repack/ctrl_root/control ]; then
  echo -e "Package: exteplayer3\nVersion: 1:181+git3\nDescription: exteplayer3 with HLS improvements\nSection: base\nPriority: optional\nMaintainer: saufsoldat\nLicense: GPL-2.0\nArchitecture: armv7ahf-vfp-neon\nDepends: libc6" > repack/ctrl_root/control
fi
if [ ! -f repack/debian-binary ]; then
  echo "2.0" > repack/debian-binary
fi
cp $BUILDDIR/exteplayer3_new repack/data_root/usr/bin/exteplayer3
chmod 755 repack/data_root/usr/bin/exteplayer3
mkdir -p repack/data_root/usr/lib/exteplayer3_deps
cp -d $FFMPEG_LIBS/lib*.so* repack/data_root/usr/lib/exteplayer3_deps/
rm -f repack/data_root/usr/lib/exteplayer3_deps/libz.so*
tar czf repack/data.tar.gz -C repack/data_root --owner=0 --group=0 ./usr
tar czf repack/control.tar.gz -C repack/ctrl_root --owner=0 --group=0 ./control
mkdir -p "$BUILDDIR/../release"
ar rcs "$BUILDDIR/../release/exteplayer3_vti_1_181+git3_armv7ahf.ipk" repack/debian-binary repack/data.tar.gz repack/control.tar.gz
echo "IPK: $(ls -lh "$BUILDDIR/../release/exteplayer3_vti_1_181+git3_armv7ahf.ipk")"

echo ""
echo "=== Installation auf Box ==="
echo "Befehl:"
echo "  cat repack/exteplayer3_vti_182_armv7ahf.ipk | ssh root@<BOX-IP> 'cat > /tmp/ep3.ipk && sed -i s/Version:.*/Version:\ 181/ /var/lib/opkg/status && opkg install --force-depends --force-overwrite /tmp/ep3.ipk'"
