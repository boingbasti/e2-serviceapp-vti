#!/bin/bash
# Packt das ffmpeg-IPK aus den Build-Artefakten
# Voraussetzung: build-ffmpeg.sh wurde erfolgreich ausgeführt
set -e

BUILDDIR="$(cd "$(dirname "$0")" && pwd)"
PKGDIR="${BUILDDIR}/ffmpeg-ipk"
PREFIX="${BUILDDIR}/ffmpeg-build-out"
OUTDIR="${BUILDDIR}/../release"

VERSION="1:181"
ARCH="armv7ahf-vfp-neon"
IPK_NAME="ffmpeg_vti_1_181_armv7ahf.ipk"

if [ ! -f "${BUILDDIR}/ffmpeg-bin/ffmpeg" ]; then
    echo "FEHLER: ffmpeg-bin/ffmpeg nicht gefunden — erst build-ffmpeg.sh ausführen!"
    exit 1
fi

echo "=== Erstelle ffmpeg-IPK ==="

rm -rf "${PKGDIR}"
mkdir -p "${PKGDIR}/data_root/usr/bin"
mkdir -p "${PKGDIR}/data_root/usr/share/ffmpeg"
mkdir -p "${PKGDIR}/ctrl_root"

# Binaries
cp "${BUILDDIR}/ffmpeg-bin/ffmpeg"  "${PKGDIR}/data_root/usr/bin/"
cp "${BUILDDIR}/ffmpeg-bin/ffprobe" "${PKGDIR}/data_root/usr/bin/"
chmod 755 "${PKGDIR}/data_root/usr/bin/ffmpeg"
chmod 755 "${PKGDIR}/data_root/usr/bin/ffprobe"

# ffpreset-Dateien aus dem Build-Output (ohne examples/)
if [ -d "${PREFIX}/share/ffmpeg" ]; then
    find "${PREFIX}/share/ffmpeg" -maxdepth 1 -type f \
        -exec cp {} "${PKGDIR}/data_root/usr/share/ffmpeg/" \;
fi

# control
cat > "${PKGDIR}/ctrl_root/control" << EOF
Package: ffmpeg
Version: ${VERSION}
Description: FFmpeg 6.1.1 built from official source for VTi images. Installs ffmpeg and ffprobe to /usr/bin/ and replaces the system ffmpeg. Shared libraries are provided by the exteplayer3 package in /usr/lib/exteplayer3_deps/.
Section: libs
Priority: optional
Maintainer: saufsoldat
License: GPL-2.0
Architecture: ${ARCH}
OE: ffmpeg
Homepage: https://www.ffmpeg.org/
Depends: libc6 (>= 2.20), exteplayer3 (= 1:181+git3)
EOF

echo "2.0" > "${PKGDIR}/debian-binary"

tar czf "${PKGDIR}/data.tar.gz"    -C "${PKGDIR}/data_root" --owner=0 --group=0 .
tar czf "${PKGDIR}/control.tar.gz" -C "${PKGDIR}/ctrl_root" --owner=0 --group=0 ./control

mkdir -p "${OUTDIR}"
ar rcs "${OUTDIR}/${IPK_NAME}" \
    "${PKGDIR}/debian-binary" \
    "${PKGDIR}/data.tar.gz" \
    "${PKGDIR}/control.tar.gz"

echo ""
echo "=== Fertig ==="
echo "IPK:     $(ls -lh "${OUTDIR}/${IPK_NAME}")"
echo "Inhalt:"
tar tzf "${PKGDIR}/data.tar.gz" | grep -v '^\.$' | sort
echo ""
echo "Control:"
cat "${PKGDIR}/ctrl_root/control"
