# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Rewrite section names, section flags and symbol names in i386 COFF objects.

Objects are rebuilt from their parts rather than patched in place, so renamed
sections and symbols may freely use the string table. Raw section data,
relocations and auxiliary symbol records are carried over unchanged; only the
offsets that locate them are recomputed. GNU format archives are read and
written with a symbol index that GNU ld and LLD both accept.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass

IMAGE_FILE_MACHINE_I386 = 0x14C

SCN_CNT_CODE = 0x00000020
SCN_CNT_INITIALIZED_DATA = 0x00000040
SCN_CNT_UNINITIALIZED_DATA = 0x00000080
SCN_LNK_INFO = 0x00000200
SCN_LNK_REMOVE = 0x00000800
SCN_LNK_COMDAT = 0x00001000
SCN_ALIGN_MASK = 0x00F00000
SCN_LNK_NRELOC_OVFL = 0x01000000
SCN_MEM_DISCARDABLE = 0x02000000
SCN_MEM_EXECUTE = 0x20000000
SCN_MEM_READ = 0x40000000
SCN_MEM_WRITE = 0x80000000

SYM_CLASS_EXTERNAL = 2
SYM_CLASS_STATIC = 3
SYM_CLASS_WEAK_EXTERNAL = 105

SYM_UNDEFINED = 0
SYM_ABSOLUTE = -1
SYM_DEBUG = -2


def alignment(flags: int) -> int:
    """The alignment encoded in section flags; COFF objects default to 16 bytes."""
    code = (flags & SCN_ALIGN_MASK) >> 20
    return 16 if code == 0 else 1 << (code - 1)


def with_alignment(flags: int, value: int) -> int:
    if value < 1 or value > 8192 or value & (value - 1):
        raise ValueError(f"invalid COFF section alignment {value}")
    return (flags & ~SCN_ALIGN_MASK) | ((value.bit_length()) << 20)


@dataclass
class Section:
    name: str
    flags: int
    size: int
    data: bytes | None  # None for uninitialized data
    relocations: bytes  # 10-byte records, without the overflow count record
    linenumbers: bytes
    linenumber_count: int


@dataclass
class Symbol:
    name: str
    value: int
    section: int  # 1-based section number, or SYM_UNDEFINED, SYM_ABSOLUTE, SYM_DEBUG
    type: int
    storage: int
    aux: bytes  # the auxiliary records that follow, 18 bytes each

    @property
    def is_definition(self) -> bool:
        """An external symbol this object defines, including commons and weak externals."""
        if self.storage == SYM_CLASS_WEAK_EXTERNAL:
            return True
        if self.storage != SYM_CLASS_EXTERNAL:
            return False
        return self.section != SYM_UNDEFINED or self.value != 0

    @property
    def is_reference(self) -> bool:
        return (
            self.storage == SYM_CLASS_EXTERNAL and self.section == SYM_UNDEFINED and self.value == 0
        )


class CoffObject:
    """An editable i386 COFF object (not bigobj, not an import library member)."""

    def __init__(self, data: bytes):
        if len(data) < 20:
            raise ValueError("truncated COFF object")
        (
            machine,
            count,
            self.timestamp,
            symbol_offset,
            symbol_count,
            optional,
            self.characteristics,
        ) = struct.unpack_from("<HHIIIHH", data, 0)
        if machine == 0 and count == 0xFFFF:
            raise ValueError("bigobj COFF objects are not supported")
        if machine != IMAGE_FILE_MACHINE_I386:
            raise ValueError(f"unsupported COFF machine {machine:#x}")
        if optional:
            raise ValueError("expected a relocatable COFF object")
        strings = b""
        if symbol_offset:
            table_end = symbol_offset + symbol_count * 18
            (string_size,) = struct.unpack_from("<I", data, table_end)
            strings = data[table_end : table_end + string_size]

        def string(offset: int) -> str:
            end = strings.index(b"\0", offset)
            return strings[offset:end].decode("utf-8", errors="surrogateescape")

        def short(raw: bytes) -> str:
            return raw.split(b"\0", 1)[0].decode("utf-8", errors="surrogateescape")

        self.sections: list[Section] = []
        for index in range(count):
            base = 20 + index * 40
            raw, _, _, size, pointer, relocs, lines, nrelocs, nlines, flags = struct.unpack_from(
                "<8sIIIIIIHHI", data, base
            )
            if raw.startswith(b"/"):
                name = string(int(raw[1:].split(b"\0", 1)[0]))
            else:
                name = short(raw)
            payload = None
            if not flags & SCN_CNT_UNINITIALIZED_DATA or pointer:
                payload = data[pointer : pointer + size] if size else b""
                if len(payload) != size:
                    raise ValueError(f"section {name} extends beyond the object")
            if flags & SCN_LNK_NRELOC_OVFL:
                (total,) = struct.unpack_from("<I", data, relocs)
                relocs += 10
                nrelocs = total - 1
            relocations = data[relocs : relocs + nrelocs * 10]
            linenumbers = data[lines : lines + nlines * 6] if nlines else b""
            self.sections.append(
                Section(
                    name,
                    flags & ~SCN_LNK_NRELOC_OVFL,
                    size,
                    payload,
                    relocations,
                    linenumbers,
                    nlines,
                )
            )

        # Symbols keep their table index: relocations and auxiliary records refer to it.
        self.symbols: list[Symbol | None] = []
        index = 0
        while index < symbol_count:
            offset = symbol_offset + index * 18
            raw, value, section, typ, storage, naux = struct.unpack_from("<8sIhHBB", data, offset)
            if raw[:4] == b"\0\0\0\0":
                name = string(struct.unpack_from("<I", raw, 4)[0])
            else:
                name = short(raw)
            aux = data[offset + 18 : offset + 18 + naux * 18]
            self.symbols.append(Symbol(name, value, section, typ, storage, aux))
            self.symbols.extend([None] * naux)
            index += 1 + naux

    def defined_symbols(self):
        return (symbol for symbol in self.symbols if symbol is not None and symbol.is_definition)

    def section_of(self, symbol: Symbol) -> Section | None:
        if symbol.section > 0:
            return self.sections[symbol.section - 1]
        return None

    def rename_section(self, index: int, name: str) -> None:
        """Rename section index (0-based) together with its section definition symbol."""
        old = self.sections[index].name
        self.sections[index].name = name
        for symbol in self.symbols:
            if (
                symbol is not None
                and symbol.section == index + 1
                and symbol.storage == SYM_CLASS_STATIC
                and symbol.value == 0
                and symbol.aux
                and symbol.name == old
            ):
                symbol.name = name

    def make_initialized(self, index: int) -> None:
        section = self.sections[index]
        if section.data is None:
            section.data = bytes(section.size)
        section.flags = (section.flags & ~SCN_CNT_UNINITIALIZED_DATA) | SCN_CNT_INITIALIZED_DATA

    def to_bytes(self) -> bytes:
        strings = bytearray(4)
        string_offsets: dict[bytes, int] = {}

        def intern(name: bytes) -> int:
            if name not in string_offsets:
                string_offsets[name] = len(strings)
                strings.extend(name + b"\0")
            return string_offsets[name]

        def section_name(name: str) -> bytes:
            encoded = name.encode("utf-8", errors="surrogateescape")
            if len(encoded) <= 8:
                return encoded.ljust(8, b"\0")
            offset = intern(encoded)
            if offset > 9999999:
                raise ValueError("COFF string table too large for a section name")
            return f"/{offset}".encode().ljust(8, b"\0")

        def symbol_name(name: str) -> bytes:
            encoded = name.encode("utf-8", errors="surrogateescape")
            if len(encoded) <= 8:
                return encoded.ljust(8, b"\0")
            return struct.pack("<II", 0, intern(encoded))

        headers = bytearray()
        body = bytearray()
        position = 20 + 40 * len(self.sections)

        def place(blob: bytes) -> int:
            nonlocal position
            if not blob:
                return 0
            padding = -position % 4
            body.extend(bytes(padding))
            position += padding
            offset = position
            body.extend(blob)
            position += len(blob)
            return offset

        for section in self.sections:
            name = section_name(section.name)
            flags = section.flags
            raw = place(section.data) if section.data else 0
            count = len(section.relocations) // 10
            relocations = section.relocations
            if count >= 0xFFFF:
                flags |= SCN_LNK_NRELOC_OVFL
                relocations = struct.pack("<IIH", count + 1, 0, 0) + relocations
                count = 0xFFFF
            reloc_offset = place(relocations)
            line_offset = place(section.linenumbers)
            headers += struct.pack(
                "<8sIIIIIIHHI",
                name,
                0,
                0,
                section.size,
                raw,
                reloc_offset,
                line_offset,
                count,
                section.linenumber_count,
                flags,
            )

        symbols = bytearray()
        for symbol in self.symbols:
            if symbol is None:
                continue
            symbols += struct.pack(
                "<8sIhHBB",
                symbol_name(symbol.name),
                symbol.value,
                symbol.section,
                symbol.type,
                symbol.storage,
                len(symbol.aux) // 18,
            )
            symbols += symbol.aux
        symbol_offset = position + (-position % 4)
        body.extend(bytes(symbol_offset - position))
        struct.pack_into("<I", strings, 0, len(strings))
        header = struct.pack(
            "<HHIIIHH",
            IMAGE_FILE_MACHINE_I386,
            len(self.sections),
            self.timestamp,
            symbol_offset,
            len(self.symbols),
            0,
            self.characteristics,
        )
        return bytes(header + headers + body + symbols + strings)


def write_object(sections, symbols) -> bytes:
    """Build a new COFF object from Section and Symbol lists."""
    obj = CoffObject.__new__(CoffObject)
    obj.timestamp = 0
    obj.characteristics = 0
    obj.sections = list(sections)
    obj.symbols = []
    for symbol in symbols:
        obj.symbols.append(symbol)
        obj.symbols.extend([None] * (len(symbol.aux) // 18))
    return obj.to_bytes()


# --- archives -----------------------------------------------------------------------


def read_archive(data: bytes) -> list[tuple[str, bytes]]:
    """Return the members of a GNU or BSD style archive, in order, without the index."""
    if not data.startswith(b"!<arch>\n"):
        raise ValueError("not an archive (thin archives are unsupported)")
    position = 8
    names = b""
    members = []
    while position < len(data):
        header = data[position : position + 60]
        if len(header) != 60 or header[58:60] != b"`\n":
            raise ValueError(f"invalid archive member header at offset {position}")
        size = int(header[48:58])
        start = position + 60
        content = data[start : start + size]
        position = start + size + (size & 1)
        name = header[:16].rstrip(b" ")
        # Indexes: GNU's, Microsoft's second one and its ARM64EC one, and BSD's.
        if name in (b"/", b"/SYM64/", b"/<ECSYMBOLS>/", b"__.SYMDEF", b"__.SYMDEF SORTED"):
            continue
        if name == b"//":
            names = content
            continue
        if name.startswith(b"#1/"):
            length = int(name[3:])
            name, content = content[:length].rstrip(b"\0"), content[length:]
        elif name.startswith(b"/") and name[1:].isdigit():
            # GNU ends long names with "/\n"; llvm-ar's COFF archives end them with NUL.
            offset = int(name[1:])
            end = min(
                (
                    stop
                    for stop in (names.find(b"\n", offset), names.find(b"\0", offset))
                    if stop >= 0
                ),
                default=-1,
            )
            if end < 0:
                raise ValueError("unterminated archive member name")
            name = names[offset:end].rstrip(b"/")
        else:
            name = name.removesuffix(b"/")
        members.append((name.decode("utf-8", errors="surrogateescape"), content))
    return members


def index_symbols(data: bytes) -> list[str]:
    """Names an archive index lists for a member: what llvm-ar would record."""
    try:
        obj = CoffObject(data)
    except ValueError:
        return []
    return [symbol.name for symbol in obj.defined_symbols()]


def write_archive(members: list[tuple[str, bytes]]) -> bytes:
    """Write a GNU archive with a symbol index, keeping member order."""

    def header(name: bytes, size: int) -> bytes:
        return (
            name.ljust(16)
            + b"0".ljust(12)
            + b"0".ljust(6)
            + b"0".ljust(6)
            + b"644".ljust(8)
            + str(size).encode().ljust(10)
            + b"`\n"
        )

    long_names = bytearray()
    encoded = []
    for name, data in members:
        raw = name.encode("utf-8", errors="surrogateescape")
        if len(raw) <= 15 and b"/" not in raw and b" " not in raw:
            encoded.append((raw + b"/", data))
        else:
            encoded.append((f"/{len(long_names)}".encode(), data))
            long_names += raw + b"/\n"
    symbols = [index_symbols(data) for _, data in members]
    names_blob = b"".join(
        name.encode("utf-8", errors="surrogateescape") + b"\0"
        for entry in symbols
        for name in entry
    )
    index_size = 4 + 4 * sum(len(entry) for entry in symbols) + len(names_blob)
    position = 8 + 60 + index_size + (index_size & 1)
    if long_names:
        position += 60 + len(long_names) + (len(long_names) & 1)
    offsets = []
    for _, data in encoded:
        offsets.append(position)
        position += 60 + len(data) + (len(data) & 1)
    out = bytearray(b"!<arch>\n")
    index = bytearray(struct.pack(">I", sum(len(entry) for entry in symbols)))
    for offset, entry in zip(offsets, symbols, strict=True):
        index += struct.pack(">I", offset) * len(entry)
    index += names_blob
    out += header(b"/", len(index)) + index + (b"\n" if len(index) & 1 else b"")
    if long_names:
        out += header(b"//", len(long_names)) + long_names + (b"\n" if len(long_names) & 1 else b"")
    for name, data in encoded:
        out += header(name, len(data)) + data + (b"\n" if len(data) & 1 else b"")
    return bytes(out)
