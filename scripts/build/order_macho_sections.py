#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Keep inherited device API sections contiguous in a Mach-O object."""

import argparse
import struct
from pathlib import Path

from gen_macho_iter_sections import shorten
from iter_sections import parse_tagged_items


def descendants(name, parent_children):
    """Return one API class and its descendants in linker order."""
    result = [name]
    for child in parent_children.get(name, []):
        result.extend(descendants(child, parent_children))
    return result


def object_metadata(data):
    """Read section headers and the symbol table of a little-endian 64-bit object."""
    if len(data) < 32:
        raise ValueError("Mach-O object is shorter than its header")
    magic, cpu, _, filetype, count, commands_size, _, _ = struct.unpack_from("<IiiIIIII", data)
    if magic != 0xFEEDFACF or filetype != 1:
        raise ValueError("Expected a little-endian 64-bit Mach-O object")
    commands_end = 32 + commands_size
    if commands_end > len(data):
        raise ValueError("Mach-O load commands extend beyond the file")
    offset = 32
    sections = []
    symtab = None
    segment = None
    for _ in range(count):
        if offset + 8 > commands_end:
            raise ValueError("Truncated Mach-O load command")
        command, size = struct.unpack_from("<II", data, offset)
        if size < 8 or offset + size > commands_end:
            raise ValueError("Invalid Mach-O load command size")
        if command == 0x19:
            if segment is not None or size < 72:
                raise ValueError("Expected one Mach-O object segment")
            segment = offset
            section_count = struct.unpack_from("<I", data, offset + 64)[0]
            if section_count > 255 or 72 + section_count * 80 > size:
                raise ValueError("Invalid Mach-O section count")
            sections = [
                bytes(data[offset + 72 + i * 80 : offset + 72 + (i + 1) * 80])
                for i in range(section_count)
            ]
        elif command == 2:
            if size < 24 or symtab is not None:
                raise ValueError("Invalid Mach-O symbol table command")
            symtab = struct.unpack_from("<IIII", data, offset + 8)
        offset += size
    if segment is None or symtab is None or offset != commands_end:
        raise ValueError("Incomplete Mach-O object metadata")
    if symtab[0] + symtab[1] * 16 > len(data) or symtab[2] + symtab[3] > len(data):
        raise ValueError("Mach-O symbol table extends beyond the file")
    return cpu, segment, sections, symtab


def section_names(sections):
    """Return the names of sections in the data segment."""
    return {
        s[:16].split(b"\0", 1)[0].decode("ascii")
        for s in sections
        if s[16:32].split(b"\0", 1)[0] == b"__DATA"
    }


def order_sections(data, parent_children, children):
    """Reorder API section headers and update all references to section indexes."""
    cpu, segment, sections, symtab = object_metadata(data)
    roots = sorted(set(parent_children) - children)
    names = [shorten(name) for root in roots for name in descendants(root, parent_children)]
    present = section_names(sections)
    names = [name for name in names if name in present]
    indexes = {s[:16].split(b"\0", 1)[0].decode("ascii"): i for i, s in enumerate(sections)}
    selected = [indexes[name] for name in names]
    if not selected:
        return bytes(data)
    first = min(selected)
    remaining = [i for i in range(first, len(sections)) if i not in selected]
    ordered = list(range(first)) + selected + remaining
    remap = {old + 1: new + 1 for new, old in enumerate(ordered)}
    output = bytearray(data)
    for new, old in enumerate(ordered):
        output[segment + 72 + new * 80 : segment + 72 + (new + 1) * 80] = sections[old]
    for section in sections:
        relocation_offset, relocation_count = struct.unpack_from("<II", section, 56)
        if relocation_offset + relocation_count * 8 > len(data):
            raise ValueError("Mach-O relocations extend beyond the file")
        for index in range(relocation_count):
            offset = relocation_offset + index * 8
            address, info = struct.unpack_from("<II", data, offset)
            if address & 0x80000000:
                raise ValueError("Scattered relocations are not supported in 64-bit objects")
            if info & (1 << 27) or (cpu == 0x100000C and info >> 28 == 10):
                continue
            target = info & 0xFFFFFF
            if target != 0:
                if target not in remap:
                    raise ValueError("Mach-O relocation has an invalid section index")
                struct.pack_into("<I", output, offset + 4, (info & 0xFF000000) | remap[target])
    symbol_offset, symbol_count, _, _ = symtab
    for index in range(symbol_count):
        offset = symbol_offset + index * 16 + 5
        target = data[offset]
        if target != 0:
            if target not in remap:
                raise ValueError("Mach-O symbol has an invalid section index")
            output[offset] = remap[target]
    return bytes(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--struct-tags", required=True, type=Path)
    args = parser.parse_args()
    _, parent_children, children = parse_tagged_items(args.struct_tags, "__subsystem")
    data = args.image.read_bytes()
    ordered = order_sections(data, parent_children, children)
    if ordered != data:
        args.image.write_bytes(ordered)


if __name__ == "__main__":
    main()
