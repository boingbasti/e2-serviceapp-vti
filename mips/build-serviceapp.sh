#!/bin/bash
# Cross-Kompilierung von serviceapp.so für VTi MIPS (VU+ Solo2, mipsel)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="${SCRIPT_DIR}/../serviceapp/src/serviceapp"
SYSROOT="${SCRIPT_DIR}/sysroot"
OUT="${SCRIPT_DIR}/out"
mkdir -p "${OUT}"

CROSS="mipsel-linux-gnu"
CC="${CROSS}-gcc"
CXX="${CROSS}-g++"

# Include-Pfade
INCLUDES=(
    "-I${SYSROOT}/usr/include"
    "-I${SYSROOT}/usr/include/python2.7"
    "-I${SYSROOT}/usr/include/enigma2"
    "-I${SRC}"
    "-I${SRC}/cJSON"
)

# Flags passend zu VTi MIPS (GCC, MIPS32r2 hard-float, Python 2.7, EABI)
CXXFLAGS=(
    "-O2"
    "-g"
    "-fPIC"
    "-mips32"
    "-mhard-float"
    "-fno-strict-aliasing"
    "-fno-delete-null-pointer-checks"
    "-fno-lifetime-dse"
    "-DDEBUG"
    "-DNO_DVB"
    "-DHAVE_EPG"
    "-DOPENPLI_ISERVICE_VERSION=2"
    "-D_GLIBCXX_USE_CXX11_ABI=0"
    "-D_USE_MATH_DEFINES"
    "-D_TIME_BITS=32"
    "-U__USE_TIME_BITS64"
    "-D_FILE_OFFSET_BITS=32"
    "-fno-rtti"
    "-include${SCRIPT_DIR}/glibc_version_pin.h"
    "${INCLUDES[@]}"
)

CFLAGS=(
    "-O2"
    "-g"
    "-fPIC"
    "-mips32"
    "-mhard-float"
    "-fno-strict-aliasing"
    "-fno-delete-null-pointer-checks"
    "-include${SCRIPT_DIR}/glibc_version_pin.h"
)

LDFLAGS=(
    "-shared"
    "-fPIC"
    "-Wl,--allow-shlib-undefined"
    "-Wl,-soname,serviceapp.so"
    "-Wl,--hash-style=sysv"
    "-Wl,-z,now"
)

CPP_SOURCES=(
    "${SCRIPT_DIR}/../compat_cxx.cpp"
    "${SRC}/common.cpp"
    "${SRC}/exteplayer3.cpp"
    "${SRC}/gstplayer.cpp"
    "${SRC}/wrappers.cpp"
    "${SRC}/extplayer.cpp"
    "${SRC}/m3u8.cpp"
    "${SRC}/myconsole.cpp"
    "${SRC}/subtitles/subtitles.cpp"
    "${SRC}/subtitles/subrip.cpp"
    "${SRC}/serviceapp.cpp"
)

# cJSON und glibc-Compat als C-Dateien
C_SOURCES=(
    "${SRC}/cJSON/cJSON.c"
    "${SCRIPT_DIR}/glibc_compat.c"
)

echo "=== Kompiliere serviceapp.so für MIPS mipsel (VTi) ==="
echo ""

# C++ Quellen kompilieren
OBJECTS=()
for src in "${CPP_SOURCES[@]}"; do
    obj="${OUT}/$(basename "${src}" .cpp).o"
    echo "  [C++] $(basename "${src}")"
    ${CXX} "${CXXFLAGS[@]}" -c "${src}" -o "${obj}" 2>&1
    OBJECTS+=("${obj}")
done

# C Quellen kompilieren (cJSON)
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
    -L"${SYSROOT}/usr/lib" \
    -lpython2.7 \
    -lssl -lcrypto \
    -luchardet \
    -lsigc-1.2 \
    -o "${OUT}/serviceapp.so" 2>&1

echo ""
echo "=== Fertig: ${OUT}/serviceapp.so ==="
mipsel-linux-gnu-file "${OUT}/serviceapp.so" 2>/dev/null || file "${OUT}/serviceapp.so"
