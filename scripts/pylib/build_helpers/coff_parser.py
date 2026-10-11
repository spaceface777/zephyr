# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Read i386 COFF objects and PE32 images used by the native simulator."""

import struct
from dataclasses import dataclass


@dataclass
class CoffSection:
    index: int
    name: str
    address: int
    size: int
    offset: int
    raw_size: int
    flags: int


@dataclass
class CoffSymbol:
    index: int
    offset: int
    name: str
    value: int
    section_index: int
    type: int
    storage: int
    aux_count: int


class CoffFile:
    """Read ordinary i386 COFF, not bigobj, import-library records, or PE32+."""

    def __init__(self, data):
        self.data = bytes(data)
        self.sections = {}
        self.symbols = {}
        self.is_image = self.data.startswith(b"MZ")
        self.image_base = 0
        header = 0
        if self.is_image:
            header = self.unpack("<I", 0x3C)[0]
            if header < 64 or self.read(header, 4) != b"PE\0\0":
                raise ValueError("Invalid PE signature")
            header += 4
        machine, count, _, symbol_offset, symbol_count, optional_size, _ = self.unpack(
            "<HHIIIHH", header
        )
        if machine != 0x14C:
            raise ValueError("Only i386 COFF/PE32 is supported")
        if self.is_image:
            if optional_size < 96 or self.unpack("<H", header + 20)[0] != 0x10B:
                raise ValueError("Expected a PE32 optional header")
            self.image_base = self.unpack("<I", header + 48)[0]
        elif optional_size != 0:
            raise ValueError("Expected a relocatable COFF object")
        section_offset = header + 20 + optional_size
        self.read(header + 20, optional_size + count * 40)
        headers_end = section_offset + count * 40
        file_alignment = 1
        if self.is_image:
            file_alignment = self.unpack("<I", header + 56)[0]
            declared_headers = self.unpack("<I", header + 80)[0]
            if (
                file_alignment == 0
                or file_alignment & (file_alignment - 1)
                or declared_headers < headers_end
            ):
                raise ValueError("Invalid PE32 file alignment or header extent")
            self.read(0, declared_headers)
            headers_end = declared_headers
        table_end = symbol_offset + symbol_count * 18
        if symbol_offset < section_offset + count * 40:
            raise ValueError("Missing or overlapping COFF symbol table")
        self.read(symbol_offset, symbol_count * 18)
        string_size = self.unpack("<I", table_end)[0]
        if string_size < 4:
            raise ValueError("Invalid COFF string table size")
        self.strings = self.read(table_end, string_size)
        raw_extents = []
        relocations = []
        for index in range(1, count + 1):
            raw, virtual_size, rva, size, offset, reloc, _, reloc_count, _, flags = self.unpack(
                "<8sIIIIIIHHI", section_offset + (index - 1) * 40
            )
            name = raw.split(b"\0", 1)[0]
            if name.startswith(b"/"):
                if not name[1:].isdigit():
                    raise ValueError("Unsupported COFF section name")
                name = self.string(int(name[1:]))
            else:
                name = name.decode("utf-8", errors="surrogateescape")
            if size != 0:
                # Relocatable BSS records its memory size without file data.
                if not self.is_image and flags & 0x80 and offset == 0:
                    pass
                else:
                    if offset < headers_end or offset % file_alignment != 0:
                        raise ValueError("Invalid COFF/PE raw section offset")
                    self.read(offset, size)
                    end = offset + size
                    if offset < table_end + string_size and end > symbol_offset:
                        raise ValueError("COFF section overlaps the symbol or string table")
                    if any(offset < stop and end > start for start, stop in raw_extents):
                        raise ValueError("Overlapping COFF/PE raw sections")
                    raw_extents.append((offset, end))
            if reloc_count != 0:
                if reloc == 0 or flags & 0x01000000:
                    raise ValueError("Unsupported COFF relocation table")
                self.read(reloc, reloc_count * 10)
                relocations.append((reloc, reloc_count, size, rva))
            self.sections[index] = CoffSection(
                index,
                name,
                self.image_base + rva,
                virtual_size if self.is_image else size,
                offset,
                size,
                flags,
            )
        index = 0
        while index < symbol_count:
            offset = symbol_offset + index * 18
            raw, value, section, typ, storage, aux = self.unpack("<8sIhHBB", offset)
            if index + aux >= symbol_count or section < -2 or section > count:
                raise ValueError("Invalid COFF symbol or auxiliary records")
            if raw[:4] == b"\0" * 4:
                name = self.string(struct.unpack_from("<I", raw, 4)[0])
            else:
                name = raw.split(b"\0", 1)[0].decode("utf-8", errors="surrogateescape")
            self.symbols[index] = CoffSymbol(index, offset, name, value, section, typ, storage, aux)
            index += aux + 1
        # MinGW emits these ordinary i386 relocations. Auxiliary symbol slots
        # are not valid relocation targets, even when their index is in range.
        widths = {0: 0, 1: 2, 2: 2, 6: 4, 7: 4, 10: 2, 11: 4, 20: 4}
        for offset, count, size, rva in relocations:
            for index in range(count):
                address, target, kind = self.unpack("<IIH", offset + index * 10)
                if kind not in widths:
                    raise ValueError(f"Unsupported i386 COFF relocation: {kind}")
                if kind == 0:
                    continue
                if (
                    target not in self.symbols
                    or address < rva
                    or address - rva + widths[kind] > size
                ):
                    raise ValueError("Invalid COFF relocation target or address")

    def read(self, offset, size):
        if offset < 0 or size < 0 or offset + size > len(self.data):
            raise ValueError("COFF/PE structure extends beyond the file")
        return self.data[offset : offset + size]

    def unpack(self, fmt, offset):
        return struct.unpack(fmt, self.read(offset, struct.calcsize(fmt)))

    def string(self, offset):
        if offset < 4 or offset >= len(self.strings):
            raise ValueError("Invalid COFF long-name offset")
        end = self.strings.find(b"\0", offset)
        if end < 0:
            raise ValueError("Unterminated COFF name")
        return self.strings[offset:end].decode("utf-8", errors="surrogateescape")

    def address(self, symbol):
        if symbol.section_index not in self.sections:
            raise ValueError("COFF symbol has no section address")
        section = self.sections[symbol.section_index]
        if symbol.value > section.size:
            raise ValueError("COFF symbol is outside its section")
        return section.address + symbol.value

    def read_pointer(self, address, index, section_index):
        if not self.is_image:
            raise ValueError("Pointers require a linked PE32 image")
        section = self.sections[section_index]
        offset = address - section.address + index * 4
        if section.offset == 0 or offset < 0 or offset + 4 > min(section.raw_size, section.size):
            raise ValueError("PE32 pointer is outside the section's file data")
        return self.unpack("<I", section.offset + offset)[0]
