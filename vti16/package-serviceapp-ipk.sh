#!/bin/bash
# Packt das serviceapp-IPK fuer VTi16 aus den Build-Artefakten
# Voraussetzung: build-serviceapp.sh wurde erfolgreich ausgefuehrt
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILDDIR="${SCRIPT_DIR}"
PKGDIR="/tmp/serviceapp-ipk-build-vti16"
OUTDIR="${SCRIPT_DIR}/../release"
PYDIR="${SCRIPT_DIR}/../serviceapp-pyfiles"

VERSION="gitAUTOINC+da9deae56b-r0.vti005+hls1"
# ARCH_TAG ist das echte, von VTi16s opkg (arch.conf) anerkannte Architektur-
# Tag (Control-Feld "Architecture:") - ein frei erfundenes Tag wie
# "armv7ahf-vti16" wird vom Solver mit "does not have a compatible
# architecture" abgelehnt, da opkg das gegen eine Whitelist prueft.
# FILE_SUFFIX ist rein fuer die eigene Dateinamens-Unterscheidung
# (install.sh waehlt IPKs per Dateiname aus, nicht per Architecture:-Feld),
# damit VTi15- und VTi16-Pakete nie versehentlich verwechselt werden.
ARCH_TAG="armv7ahf-neon"
FILE_SUFFIX="armv7ahf-vti16"
IPK_NAME="enigma2-plugin-systemplugins-serviceapp_vti005-hls1_${FILE_SUFFIX}.ipk"
INSTDIR="/usr/lib/enigma2/python/Plugins/SystemPlugins/ServiceApp"

if [ ! -f "${BUILDDIR}/out/serviceapp.so" ]; then
    echo "FEHLER: out/serviceapp.so nicht gefunden — erst build-serviceapp.sh ausführen!"
    exit 1
fi

if [ ! -d "${PYDIR}" ]; then
    echo "FEHLER: ${PYDIR} nicht gefunden — Python-Dateien fehlen!"
    exit 1
fi

echo "=== Erstelle serviceapp-IPK für VTi16 ==="

rm -rf "${PKGDIR}"
mkdir -p "${PKGDIR}/data_root${INSTDIR}"
mkdir -p "${PKGDIR}/ctrl_root"
mkdir -p "${OUTDIR}"

arm-linux-gnueabihf-strip -o "${PKGDIR}/data_root${INSTDIR}/serviceapp.so" "${BUILDDIR}/out/serviceapp.so"
chmod 755 "${PKGDIR}/data_root${INSTDIR}/serviceapp.so"

cp "${PYDIR}/__init__.py"          "${PKGDIR}/data_root${INSTDIR}/"
cp "${PYDIR}/plugin.py"            "${PKGDIR}/data_root${INSTDIR}/"
cp "${PYDIR}/serviceapp_client.py" "${PKGDIR}/data_root${INSTDIR}/"
cp "${PYDIR}/serviceapp_caps.py"   "${PKGDIR}/data_root${INSTDIR}/"

# Depends-Versionsfloors gegen die echten, auf einer VTi16-Testbox
# installierten Versionen geprueft (opkg list-installed/opkg search), nicht geschaetzt:
# libc6 2.40+git..., libstdc++6/libgcc1 14.2.0, libssl1.0.2/libcrypto1.0.2
# 1.0.2u (Paketname "libssl1.0.2", NICHT "libssl1.0.0" wie auf VTi15),
# uchardet 0.0.8. "openssl10-conf" wird von libssl1.0.2 automatisch
# nachgezogen, kein eigener Depends noetig.
cat > "${PKGDIR}/ctrl_root/control" << EOF
Package: enigma2-plugin-systemplugins-serviceapp
Version: ${VERSION}
Description: ServiceApp enigma2 plugin with HLS audio track support and RFC3986 URL fixes (VTi16-Build).
Section: base
Priority: optional
Maintainer: saufsoldat
License: GPL-2.0
Homepage: https://github.com/boingbasti/e2-serviceapp-vti
Architecture: ${ARCH_TAG}
Depends: gstplayer, libgcc1 (>= 14.2.0), libssl1.0.2 (>= 1.0.2u), libstdc++6 (>= 14.2.0), enigma2, openssl, libcrypto1.0.2 (>= 1.0.2u), exteplayer3, libc6 (>= 2.40), uchardet (>= 0.0.8)
Recommends: exteplayer3 (>= 1:182+vti001)
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
echo "IPK: $(ls -lh "${OUTDIR}/${IPK_NAME}")"
