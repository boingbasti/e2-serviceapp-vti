#!/bin/bash
# Packt das ffmpeg-IPK für VTi16 aus den Build-Artefakten
# Voraussetzung: build-ffmpeg.sh wurde erfolgreich ausgeführt
set -e

BUILDDIR="$(cd "$(dirname "$0")" && pwd)"
PKGDIR="${BUILDDIR}/ffmpeg-ipk"
PREFIX="${BUILDDIR}/ffmpeg-build-out"
OUTDIR="${BUILDDIR}/../../release"

VERSION="1:182"
# Siehe package-serviceapp-ipk.sh fuer die Begruendung der Trennung
# zwischen echtem opkg-Architektur-Tag und reinem Dateinamens-Suffix.
ARCH_TAG="armv7ahf-neon"
FILE_SUFFIX="armv7ahf-vti16"
IPK_NAME="ffmpeg_vti_1_182_${FILE_SUFFIX}.ipk"

if [ ! -f "${BUILDDIR}/ffmpeg-bin/ffmpeg" ]; then
    echo "FEHLER: ffmpeg-bin/ffmpeg nicht gefunden — erst build-ffmpeg.sh ausführen!"
    exit 1
fi

echo "=== Erstelle ffmpeg-IPK für VTi16 ==="

rm -rf "${PKGDIR}"
mkdir -p "${PKGDIR}/data_root/usr/bin"
mkdir -p "${PKGDIR}/data_root/usr/share/ffmpeg"
mkdir -p "${PKGDIR}/ctrl_root"

cp "${BUILDDIR}/ffmpeg-bin/ffmpeg"  "${PKGDIR}/data_root/usr/bin/"
cp "${BUILDDIR}/ffmpeg-bin/ffprobe" "${PKGDIR}/data_root/usr/bin/"
chmod 755 "${PKGDIR}/data_root/usr/bin/ffmpeg"
chmod 755 "${PKGDIR}/data_root/usr/bin/ffprobe"

if [ -d "${PREFIX}/share/ffmpeg" ]; then
    find "${PREFIX}/share/ffmpeg" -maxdepth 1 -type f \
        -exec cp {} "${PKGDIR}/data_root/usr/share/ffmpeg/" \;
fi

cat > "${PKGDIR}/ctrl_root/control" << EOF
Package: ffmpeg
Version: ${VERSION}
Description: FFmpeg 7.0.2 built from official source for VTi16 images. Installs ffmpeg and ffprobe to /usr/bin/ and replaces the system ffmpeg. Shared libraries are provided by the exteplayer3 package in /usr/lib/exteplayer3_deps/.
Section: libs
Priority: optional
Maintainer: saufsoldat
License: GPL-2.0
Architecture: ${ARCH_TAG}
OE: ffmpeg
Homepage: https://www.ffmpeg.org/
Depends: libc6 (>= 2.40), exteplayer3 (= 1:182+vti001), libxml2
Source: https://ffmpeg.org/releases/ffmpeg-7.0.2.tar.xz
EOF

cat > "${PKGDIR}/ctrl_root/postrm" << 'EOF'
#!/bin/sh
if [ "$1" = "remove" ]; then
    echo ""
    echo "Hinweis: ffmpeg wurde entfernt. Andere Plugins, die ffmpeg benoetigen,"
    echo "funktionieren erst wieder, wenn ffmpeg erneut installiert ist:"
    echo "  Original-Version: opkg install ffmpeg"
    echo "  Diese Version:    alle drei IPKs aus dem ZIP erneut installieren (install.sh oder manuell)"
    echo ""
fi
EOF
chmod 755 "${PKGDIR}/ctrl_root/postrm"

echo "2.0" > "${PKGDIR}/debian-binary"

tar czf "${PKGDIR}/data.tar.gz"    -C "${PKGDIR}/data_root" --owner=0 --group=0 .
tar czf "${PKGDIR}/control.tar.gz" -C "${PKGDIR}/ctrl_root" --owner=0 --group=0 ./control ./postrm

mkdir -p "${OUTDIR}"
ar rcs "${OUTDIR}/${IPK_NAME}" \
    "${PKGDIR}/debian-binary" \
    "${PKGDIR}/data.tar.gz" \
    "${PKGDIR}/control.tar.gz"

echo ""
echo "=== Fertig ==="
echo "IPK: $(ls -lh "${OUTDIR}/${IPK_NAME}")"
