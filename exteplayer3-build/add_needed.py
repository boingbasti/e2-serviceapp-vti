#!/usr/bin/env python3
"""Fügt eine NEEDED-Bibliothek zu einem ELF-Binary hinzu (ARM 32-bit LE).
   Funktioniert nur wenn ausreichend Platz in der Dynamic-Sektion vorhanden ist."""
import sys, struct

def add_needed(path, libname):
    with open(path, 'rb') as f:
        data = bytearray(f.read())

    # ELF-Header (32-bit LE)
    e_shoff     = struct.unpack_from('<I', data, 0x20)[0]
    e_shentsize = struct.unpack_from('<H', data, 0x2e)[0]
    e_shnum     = struct.unpack_from('<H', data, 0x30)[0]
    e_shstrndx  = struct.unpack_from('<H', data, 0x32)[0]
    e_phoff     = struct.unpack_from('<I', data, 0x1c)[0]
    e_phentsize = struct.unpack_from('<H', data, 0x2a)[0]
    e_phnum     = struct.unpack_from('<H', data, 0x2c)[0]

    shstr_sh    = e_shoff + e_shstrndx * e_shentsize
    shstr_off   = struct.unpack_from('<I', data, shstr_sh + 0x10)[0]

    SHT_DYNAMIC = 7
    SHT_STRTAB  = 3
    DT_NEEDED   = 1
    DT_STRSZ    = 10
    DT_NULL     = 0

    dyn_off = dyn_size = 0
    dynstr_off = dynstr_size = 0

    for i in range(e_shnum):
        sh = e_shoff + i * e_shentsize
        sh_type   = struct.unpack_from('<I', data, sh + 0x04)[0]
        sh_offset = struct.unpack_from('<I', data, sh + 0x10)[0]
        sh_size   = struct.unpack_from('<I', data, sh + 0x14)[0]
        sh_name   = data[shstr_off + struct.unpack_from('<I', data, sh)[0]:].split(b'\x00')[0].decode(errors='replace')
        if sh_type == SHT_DYNAMIC:
            dyn_off, dyn_size = sh_offset, sh_size
        if sh_type == SHT_STRTAB and sh_name == '.dynstr':
            dynstr_off, dynstr_size = sh_offset, sh_size

    if not dyn_off or not dynstr_off:
        print("FEHLER: .dynamic oder .dynstr nicht gefunden"); sys.exit(1)

    # Prüfen ob libname schon drin ist
    dynstr_data = data[dynstr_off:dynstr_off + dynstr_size]
    needle = libname.encode() + b'\x00'
    if needle in dynstr_data:
        # Finde den Index
        idx = dynstr_data.index(needle)
        # Prüfe ob NEEDED-Eintrag schon existiert
        pos = dyn_off
        while pos < dyn_off + dyn_size:
            tag = struct.unpack_from('<I', data, pos)[0]
            val = struct.unpack_from('<I', data, pos + 4)[0]
            if tag == DT_NEEDED and val == idx:
                print(f"{libname} ist bereits als NEEDED eingetragen"); return
            if tag == DT_NULL: break
            pos += 8
        str_idx = idx
    else:
        # Libname an .dynstr anhängen — braucht Platz!
        # Ich suche, ob nach dynstr noch Nullbytes als Puffer sind
        end = dynstr_off + dynstr_size
        if data[end:end+len(needle)] != b'\x00' * len(needle):
            print("FEHLER: Kein Platz in .dynstr zum Anhängen"); sys.exit(1)
        str_idx = dynstr_size
        data[end:end+len(needle)] = needle
        # dynstr size in Section-Header erhöhen
        for i in range(e_shnum):
            sh = e_shoff + i * e_shentsize
            sh_type   = struct.unpack_from('<I', data, sh + 0x04)[0]
            sh_offset = struct.unpack_from('<I', data, sh + 0x10)[0]
            sh_name   = data[shstr_off + struct.unpack_from('<I', data, sh)[0]:].split(b'\x00')[0].decode(errors='replace')
            if sh_type == SHT_STRTAB and sh_name == '.dynstr':
                struct.pack_into('<I', data, sh + 0x14, dynstr_size + len(needle))
                break

    # DT_NULL-Eintrag in .dynamic durch neuen DT_NEEDED ersetzen,
    # dann einen neuen DT_NULL dahinter setzen
    pos = dyn_off
    null_pos = None
    while pos < dyn_off + dyn_size:
        tag = struct.unpack_from('<I', data, pos)[0]
        if tag == DT_NULL:
            null_pos = pos
            break
        pos += 8

    if null_pos is None or null_pos + 16 > dyn_off + dyn_size:
        print("FEHLER: Kein Platz in .dynamic für neuen NEEDED-Eintrag"); sys.exit(1)

    struct.pack_into('<II', data, null_pos, DT_NEEDED, str_idx)
    struct.pack_into('<II', data, null_pos + 8, DT_NULL, 0)

    with open(path, 'wb') as f:
        f.write(data)
    print(f"{path}: NEEDED {libname} hinzugefügt (str_idx={str_idx})")

if __name__ == '__main__':
    add_needed(sys.argv[1], sys.argv[2])
