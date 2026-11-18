#!/bin/bash
# Packt das serviceapp-IPK für MIPS aus den Build-Artefakten
# Voraussetzung: build-serviceapp.sh wurde erfolgreich ausgeführt
set -e

MIPSDIR="$(cd "$(dirname "$0")" && pwd)"
BUILDDIR="${MIPSDIR}/.."
PKGDIR="/tmp/serviceapp-ipk-build-mips"
OUTDIR="${BUILDDIR}/release"
PYDIR="${BUILDDIR}/serviceapp/src/plugin"

VERSION="gitAUTOINC+da9deae56b-r0.vti005+hls1"
ARCH="mips32el"
ARCH_SHORT="mips32el"
IPK_NAME="enigma2-plugin-systemplugins-serviceapp_vti005-hls1_${ARCH_SHORT}.ipk"
INSTDIR="/usr/lib/enigma2/python/Plugins/SystemPlugins/ServiceApp"

if [ ! -f "${MIPSDIR}/out/serviceapp.so" ]; then
    echo "FEHLER: mips/out/serviceapp.so nicht gefunden — erst build-serviceapp.sh ausführen!"
    exit 1
fi

if [ ! -d "${PYDIR}" ]; then
    echo "FEHLER: ${PYDIR} nicht gefunden — Python-Dateien fehlen!"
    exit 1
fi

echo "=== Erstelle serviceapp-IPK für MIPS ==="

rm -rf "${PKGDIR}"
mkdir -p "${PKGDIR}/data_root${INSTDIR}"
mkdir -p "${PKGDIR}/ctrl_root"
mkdir -p "${OUTDIR}"

# serviceapp.so strip
mipsel-linux-gnu-strip -o "${PKGDIR}/data_root${INSTDIR}/serviceapp.so" "${MIPSDIR}/out/serviceapp.so"
python3 -c "with open('${PKGDIR}/data_root${INSTDIR}/serviceapp.so', 'r+b') as f: f.seek(8); f.write(b'\x00')"
chmod 755 "${PKGDIR}/data_root${INSTDIR}/serviceapp.so"

# Python-Dateien
cp "${PYDIR}/__init__.py"        "${PKGDIR}/data_root${INSTDIR}/"
cp "${PYDIR}/plugin.py"          "${PKGDIR}/data_root${INSTDIR}/"
cp "${PYDIR}/serviceapp_client.py" "${PKGDIR}/data_root${INSTDIR}/"
cp "${PYDIR}/serviceapp_caps.py" "${PKGDIR}/data_root${INSTDIR}/"

cat > "${PKGDIR}/ctrl_root/control" << EOF
Package: enigma2-plugin-systemplugins-serviceapp
Version: ${VERSION}
Description: ServiceApp enigma2 plugin with HLS audio track support and RFC3986 URL fixes.
Section: base
Priority: optional
Maintainer: saufsoldat
License: GPL-2.0
Homepage: https://github.com/boingbasti/e2-serviceapp-vti
Architecture: ${ARCH}
Depends: gstplayer, libgcc1 (>= 4.9.2), libssl1.0.0 (>= 1.0.2a), libstdc++6 (>= 4.9.2), enigma2, openssl, libcrypto1.0.0 (>= 1.0.2a), exteplayer3, libc6 (>= 2.20), uchardet (>= 0.0.6)
Source: git://github.com/mx3L/serviceapp.git;branch=master;protocol=https
EOF

cat > "${PKGDIR}/ctrl_root/postinst" << EOF
#!/bin/sh
rm -f ${INSTDIR}/*.pyo ${INSTDIR}/*.pyc
EOF
chmod 755 "${PKGDIR}/ctrl_root/postinst"

echo "2.0" > "${PKGDIR}/debian-binary"

tar czf "${PKGDIR}/data.tar.gz"    -C "${PKGDIR}/data_root" --owner=0 --group=0 .
tar czf "${PKGDIR}/control.tar.gz" -C "${PKGDIR}/ctrl_root" --owner=0 --group=0 ./control ./postinst

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
rm -rf "${PKGDIR}"
