# -*- coding: utf-8 -*-
"""极简 ELF32/64 解析：打印 DT_NEEDED 与 PT_INTERP。"""
import sys, struct

def read_elf_needed(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'\x7fELF'
    is64 = d[4] == 2
    le = d[5] == 1
    fmt = '<' if le else '>'
    if is64:
        (e_type, e_machine, e_ver, e_entry, e_phoff, e_shoff, e_flags,
         e_ehsize, e_phentsize, e_phnum) = struct.unpack_from(fmt + 'HHIQQQIHHH', d, 16)
    else:
        (e_type, e_machine, e_ver, e_entry, e_phoff, e_shoff, e_flags,
         e_ehsize, e_phentsize, e_phnum) = struct.unpack_from(fmt + 'HHIIIIIHHH', d, 16)
    print(f'{"ELF64" if is64 else "ELF32"} machine={e_machine} type={e_type}')
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        if is64:
            p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from(fmt + 'IIQQQQQQ', d, off)
        else:
            p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack_from(fmt + 'IIIIIIII', d, off)
        if p_type == 3:  # INTERP
            print('INTERP:', d[p_offset:p_offset+p_filesz].rstrip(b'\x00').decode())
        if p_type == 2:  # DYNAMIC
            dyn = []
            j = p_offset
            while True:
                if is64:
                    tag, val = struct.unpack_from(fmt + 'qQ', d, j)
                    j += 16
                else:
                    tag, val = struct.unpack_from(fmt + 'iI', d, j)
                    j += 8
                if tag == 0:
                    break
                dyn.append((tag, val))
            needed = [v for t, v in dyn if t == 1]
            strtab_off = [v for t, v in dyn if t == 5]
            soname = [v for t, v in dyn if t == 14]
            base = None
            # 找字符串表在文件中的偏移：用 strtab 虚拟地址对应的文件偏移（近似：在 dynamic 段内找）
            st = None
            for t, v in dyn:
                if t == 5:
                    # strtab vaddr -> 通过 program headers 换算
                    for k in range(e_phnum):
                        o2 = e_phoff + k * e_phentsize
                        if is64:
                            t2, f2, o3, v2, p2, fs2, ms2, a2 = struct.unpack_from(fmt + 'IIQQQQQQ', d, o2)
                        else:
                            t2, o3, v2, p2, fs2, ms2, f2, a2 = struct.unpack_from(fmt + 'IIIIIIII', d, o2)
                        if t2 == 1 and v2 <= v < v2 + fs2:  # PT_LOAD
                            st = o3 + (v - v2)
                            break
            if st is not None:
                for v in needed:
                    end = d.index(b'\x00', st + v)
                    print('NEEDED:', d[st+v:end].decode())
                for v in soname:
                    end = d.index(b'\x00', st + v)
                    print('SONAME:', d[st+v:end].decode())
            else:
                print('strtab vaddr not mapped')

if __name__ == '__main__':
    read_elf_needed(sys.argv[1])
