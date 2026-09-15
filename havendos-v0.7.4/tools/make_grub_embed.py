#!/usr/bin/env python3
"""
make_grub_embed.py — generates include/grub_embed.h
Usage: make_grub_embed.py boot.img core.img output.h
"""
import sys, os

boot_file = sys.argv[1]
core_file = sys.argv[2]
out_file  = sys.argv[3]

def file_to_c_array(path, name):
    with open(path, 'rb') as f:
        data = f.read()
    lines = [f'/* {os.path.basename(path)}: {len(data)} bytes */']
    lines.append(f'static const uint8_t {name}[] = {{')
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_vals = ', '.join(f'0x{b:02X}' for b in chunk)
        lines.append(f'    {hex_vals},')
    lines.append('};')
    lines.append(f'static const uint32_t {name}_size = {len(data)};')
    return '\n'.join(lines)

with open(out_file, 'w') as f:
    f.write('#ifndef GRUB_EMBED_H\n#define GRUB_EMBED_H\n#include "types.h"\n\n')
    f.write(file_to_c_array(boot_file, 'grub_boot_img'))
    f.write('\n\n')
    f.write(file_to_c_array(core_file, 'grub_core_img'))
    f.write('\n\n#endif /* GRUB_EMBED_H */\n')

print(f"Generated {out_file}: boot={os.path.getsize(boot_file)}b core={os.path.getsize(core_file)}b")
