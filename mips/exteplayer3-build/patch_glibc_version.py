#!/usr/bin/env python3
"""Patcht neuere GLIBC-Versionen auf glibc 2.0-kompatible Versionen (MIPS 32-bit LE)."""
import sys
import struct

# (alt, neu) — müssen exakt gleiche Bytezahl inkl. Null-Terminator haben
VERSION_MAP = [
    (b'GLIBC_2.38\x00', b'GLIBC_2.0\x00\x00'),
    (b'GLIBC_2.35\x00', b'GLIBC_2.0\x00\x00'),
    (b'GLIBC_2.34\x00', b'GLIBC_2.0\x00\x00'),
    (b'GLIBC_2.33\x00', b'GLIBC_2.0\x00\x00'),
    (b'GLIBC_2.32\x00', b'GLIBC_2.0\x00\x00'),
    (b'GLIBC_2.29\x00', b'GLIBC_2.0\x00\x00'),
    (b'GLIBC_2.27\x00', b'GLIBC_2.0\x00\x00'),
    (b'GLIBC_2.25\x00', b'GLIBC_2.0\x00\x00'),
    (b'GLIBC_2.23\x00', b'GLIBC_2.0\x00\x00'),
    (b'GLIBC_2.22\x00', b'GLIBC_2.0\x00\x00'),
]

def elf_hash(name):
    h = 0
    for c in name.encode():
        h = (h << 4) + c
        g = h & 0xf0000000
        if g:
            h ^= g >> 24
        h &= ~g
        h &= 0xffffffff
    return h

def patch(path):
    with open(path, 'rb') as f:
        data = bytearray(f.read())

    e_shoff     = struct.unpack_from('<I', data, 0x20)[0]
    e_shentsize = struct.unpack_from('<H', data, 0x2e)[0]
    e_shnum     = struct.unpack_from('<H', data, 0x30)[0]
    e_shstrndx  = struct.unpack_from('<H', data, 0x32)[0]

    shstr_off = e_shoff + e_shstrndx * e_shentsize
    shstr_data_off = struct.unpack_from('<I', data, shstr_off + 0x10)[0]

    SHT_VERNEED = 0x6ffffffe
    SHT_STRTAB  = 3

    verneed_off = verneed_size = 0
    strtab_off  = strtab_size  = 0

    for i in range(e_shnum):
        sh = e_shoff + i * e_shentsize
        sh_type    = struct.unpack_from('<I', data, sh + 0x04)[0]
        sh_offset  = struct.unpack_from('<I', data, sh + 0x10)[0]
        sh_size    = struct.unpack_from('<I', data, sh + 0x14)[0]
        sh_name_idx = struct.unpack_from('<I', data, sh)[0]
        sh_name = data[shstr_data_off + sh_name_idx:].split(b'\x00')[0].decode(errors='replace')

        if sh_type == SHT_VERNEED:
            verneed_off, verneed_size = sh_offset, sh_size
        if sh_type == SHT_STRTAB and sh_name == '.dynstr':
            strtab_off, strtab_size = sh_offset, sh_size

    if not verneed_off or not strtab_off:
        print(f"WARNUNG: VERNEED oder .dynstr nicht gefunden in {path} (evtl. kein Dynamic-linking)")
        return

    new_hash = elf_hash('GLIBC_2.0')
    total_strings = 0
    total_hashes  = 0

    for OLD, NEW in VERSION_MAP:
        old_ver = OLD.rstrip(b'\x00').decode()
        old_hash = elf_hash(old_ver)

        # Strings in .dynstr patchen
        patched_strings = 0
        pos = strtab_off
        while pos < strtab_off + strtab_size - len(OLD):
            if data[pos:pos+len(OLD)] == OLD:
                data[pos:pos+len(NEW)] = NEW
                patched_strings += 1
            pos += 1

        # Hashes in VERNEED-Sektion patchen
        # Elf32_Vernaux: vna_hash(4), vna_flags(2), vna_other(2), vna_name(4), vna_next(4)
        patched_hashes = 0
        pos = verneed_off
        while pos < verneed_off + verneed_size:
            vn_cnt  = struct.unpack_from('<H', data, pos + 2)[0]
            vn_aux  = struct.unpack_from('<I', data, pos + 8)[0]
            vn_next = struct.unpack_from('<I', data, pos + 12)[0]
            aux_pos = pos + vn_aux
            for _ in range(vn_cnt):
                vna_hash = struct.unpack_from('<I', data, aux_pos)[0]
                if vna_hash == old_hash:
                    struct.pack_into('<I', data, aux_pos, new_hash)
                    patched_hashes += 1
                vna_next = struct.unpack_from('<I', data, aux_pos + 12)[0]
                if not vna_next:
                    break
                aux_pos += vna_next
            if not vn_next:
                break
            pos += vn_next

        if patched_strings or patched_hashes:
            print(f"  {old_ver} → GLIBC_2.0: {patched_strings} String(s), {patched_hashes} Hash(es)")
        total_strings += patched_strings
        total_hashes  += patched_hashes

    if total_strings == 0 and total_hashes == 0:
        return

    with open(path, 'wb') as f:
        f.write(data)
    print(f"{path}: {total_strings} String(s), {total_hashes} Hash(es) gepatcht → GLIBC_2.0")

if __name__ == '__main__':
    for p in sys.argv[1:]:
        patch(p)
