#!/bin/bash
# Cross-Kompilierung von serviceapp.so für VTi16 (VU+ Duo 4K Lite/SE, ARM)
#
# Aenderungen gegenueber dem VTi15-Pendant (build-serviceapp.sh im
# Repo-Wurzelverzeichnis):
# - Kein -D_GLIBCXX_USE_CXX11_ABI=0 mehr (GCC-14-Standard 1 bleibt aktiv,
#   VTi16s enigma2 exportiert __cxx11-gemangelte Symbole).
# - -D_TIME_BITS=64 -D_FILE_OFFSET_BITS=64 statt 32 (VTi16 nutzt 64-Bit
#   time_t, per strings/objdump am echten Binary verifiziert).
# - Kein -include glibc_version_pin.h (nicht noetig, VTi16s glibc ist neu genug).
# - Kein compat_cxx.cpp in den Quellen (GCC4.9-Aera-Shim, VTi16s libstdc++
#   ist neuer als das, wofuer er urspruenglich gebraucht wurde).
# - Kein glibc_compat.c in den Quellen (keine Symbolversions-Pins noetig).
#   Falls der Link-/Ladeschritt doch ein fehlendes Symbol zeigt, hier
#   gezielt nachziehen, nicht pauschal.
# - Der dynamische Linker (ld-linux-armhf.so.3) muss als zusaetzliche
#   Link-Eingabe angegeben werden (aufloest sonst fehlende interne
#   GLIBC_PRIVATE-Symbole wie __nptl_change_stack_perm).

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="${SCRIPT_DIR}/../serviceapp/src/serviceapp"
SYSROOT="${SCRIPT_DIR}/sysroot"
OUT="${SCRIPT_DIR}/out"
mkdir -p "${OUT}"

CROSS="arm-linux-gnueabihf"
CC="${CROSS}-gcc"
CXX="${CROSS}-g++"

INCLUDES=(
    "-I${SYSROOT}/usr/include"
    "-I${SYSROOT}/usr/include/python2.7"
    "-I${SYSROOT}/usr/include/enigma2"
    "-I${SRC}"
    "-I${SRC}/cJSON"
    "-I${SCRIPT_DIR}/exteplayer3-build/ffmpeg-7.0.2"
)

CXXFLAGS=(
    "-O2"
    "-g"
    "-fPIC"
    "-march=armv7-a"
    "-mno-unaligned-access"
    "-mfpu=neon"
    "-mfloat-abi=hard"
    "-fno-strict-aliasing"
    "-fno-delete-null-pointer-checks"
    "-fno-lifetime-dse"
    "-DDEBUG"
    "-DNO_DVB"
    "-DHAVE_EPG"
    "-DOPENPLI_ISERVICE_VERSION=2"
    "-D_USE_MATH_DEFINES"
    "-D_TIME_BITS=64"
    "-D_FILE_OFFSET_BITS=64"
    "-fno-rtti"
    "${INCLUDES[@]}"
)

CFLAGS=(
    "-O2"
    "-g"
    "-fPIC"
    "-march=armv7-a"
    "-mno-unaligned-access"
    "-mfpu=neon"
    "-mfloat-abi=hard"
    "-fno-strict-aliasing"
    "-fno-delete-null-pointer-checks"
)

LDFLAGS=(
    "-shared"
    "-fPIC"
    "-Wl,--allow-shlib-undefined"
    "-Wl,-soname,serviceapp.so"
)

CPP_SOURCES=(
    "${SRC}/common.cpp"
    "${SRC}/exteplayer3.cpp"
    "${SRC}/gstplayer.cpp"
    "${SRC}/wrappers.cpp"
    "${SRC}/extplayer.cpp"
    "${SRC}/m3u8.cpp"
    "${SRC}/myconsole.cpp"
    "${SRC}/subtitles/subtitles.cpp"
    "${SRC}/subtitles/subrip.cpp"
    "${SRC}/ffprobe/ffprobe_length.cpp"
    "${SRC}/serviceapprecord.cpp"
    "${SRC}/serviceapp.cpp"
)

C_SOURCES=(
    "${SRC}/cJSON/cJSON.c"
)

echo "=== Kompiliere serviceapp.so für ARM armhf (VTi16) ==="
echo ""

OBJECTS=()
for src in "${CPP_SOURCES[@]}"; do
    obj="${OUT}/$(basename "${src}" .cpp).o"
    echo "  [C++] $(basename "${src}")"
    ${CXX} "${CXXFLAGS[@]}" -c "${src}" -o "${obj}" 2>&1
    OBJECTS+=("${obj}")
done

for src in "${C_SOURCES[@]}"; do
    obj="${OUT}/$(basename "${src}" .c).o"
    echo "  [CC]  $(basename "${src}")"
    ${CC} "${CFLAGS[@]}" "${INCLUDES[@]}" -c "${src}" -o "${obj}" 2>&1
    OBJECTS+=("${obj}")
done

echo ""
echo "  [LD] serviceapp.so"
${CXX} "${LDFLAGS[@]}" \
    "${OBJECTS[@]}" \
    -L"${SYSROOT}/usr/lib/arm" \
    "${SYSROOT}/usr/lib/arm/ld-linux-armhf.so.3" \
    -lpython2.7 \
    -lssl -lcrypto \
    -luchardet \
    -lsigc-1.2 \
    -ldl \
    -o "${OUT}/serviceapp.so" 2>&1

echo ""
echo "=== Fertig: ${OUT}/serviceapp.so ==="
arm-linux-gnueabihf-file "${OUT}/serviceapp.so" 2>/dev/null || file "${OUT}/serviceapp.so"
