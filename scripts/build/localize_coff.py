#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Localize an i386 native image, preserving COFF weak-external semantics."""

import argparse
import os
import pathlib
import struct
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "pylib" / "build_helpers"))
from coff_parser import CoffFile  # noqa: E402


def archive_members(data):
    """Yield ordinary archive members without extracting duplicate names.

    GNU ar ends long member names with "/\\n", llvm-ar's COFF archives with NUL.
    """
    if not data.startswith(b"!<arch>\n"):
        raise ValueError("Expected an ordinary archive; thin archives are unsupported")
    pos, names = 8, None
    while pos < len(data):
        header = data[pos : pos + 60]
        if len(header) != 60 or header[58:] != b"`\n" or not header[48:58].strip().isdigit():
            raise ValueError(f"Invalid archive header at {pos}")
        size = int(header[48:58])
        start, end = pos + 60, pos + 60 + size
        if end + (size & 1) > len(data) or (size & 1 and data[end : end + 1] != b"\n"):
            raise ValueError(f"Truncated archive member at {pos}")
        member_offset = pos
        pos = end + (size & 1)
        name = header[:16].rstrip(b" ")
        if name in (b"/", b"/SYM64/", b"/<ECSYMBOLS>/"):
            continue
        if name == b"//":
            if names is not None:
                raise ValueError("Duplicate archive name table")
            names = data[start:end]
            continue
        if name.startswith(b"/"):
            digits = name[1:]
            if not digits.isdigit() or names is None:
                raise ValueError("Invalid GNU archive name")
            index = int(digits)
            if index >= len(names) or (index != 0 and names[index - 1 : index] not in b"\n\0"):
                raise ValueError("Invalid archive name offset")
            stops = [
                stop for stop in (names.find(b"/\n", index), names.find(b"\0", index)) if stop >= 0
            ]
            if not stops:
                raise ValueError("Unterminated archive name")
            name = names[index : min(stops)]
        else:
            name = name.removesuffix(b"/")
        if not name or name.startswith(b"#1/"):
            raise ValueError("Unsupported archive member name")
        yield f"{name.decode('utf-8', errors='replace')}@{member_offset}", data[start:end]


def weak_target(obj, symbol):
    if symbol.section_index != 0 or symbol.aux_count != 1:
        raise ValueError(f"Unsupported COFF weak record: {symbol.name}")
    target, mode = obj.unpack("<II", symbol.offset + 18)
    # GCC writes IMAGE_WEAK_EXTERN_SEARCH_NOLIBRARY (1) and Clang
    # IMAGE_WEAK_EXTERN_SEARCH_ALIAS (3). GNU ld links both as NOLIBRARY.
    if mode not in (1, 3):
        raise ValueError(f"Unsupported COFF weak search mode: {symbol.name}")
    return target, mode


def provenance(paths):
    result = {}
    for path in paths:
        data = path.read_bytes()
        members = archive_members(data) if data.startswith(b"!<") else [("", data)]
        for member, data in members:
            try:
                obj = CoffFile(data)
                if obj.is_image:
                    raise ValueError("Expected relocatable input")
                for symbol in obj.symbols.values():
                    if symbol.storage != 105:
                        continue
                    index, mode = weak_target(obj, symbol)
                    fallback = obj.symbols.get(index)
                    if (
                        fallback is None
                        or fallback.storage not in (2, 3)
                        or fallback.section_index in (0, -2)
                    ):
                        raise ValueError(f"Invalid weak fallback: {symbol.name}")
                    if fallback.section_index > 0:
                        obj.address(fallback)
                    result.setdefault(symbol.name, set()).add((fallback.name, mode))
            except ValueError as exc:
                raise ValueError(f"{path}({member}): {exc}") from exc
    return result


def interface(obj, symbol):
    section = obj.sections.get(symbol.section_index)
    return section is not None and section.name.startswith(".native_sim_if")


def repair(obj, evidence):
    """Repair ld -r's stale TagIndex from exact original auxiliary metadata.

    See https://sourceware.org/legacy-ml/binutils/2015-10/msg00236.html.
    Private definitions are already resolved inside the embedded image; public
    weak interfaces and undefined host references must remain overridable.
    """
    if obj.is_image:
        raise ValueError("Expected a relocatable embedded image")
    data = bytearray(obj.data)
    definitions = {}
    public = set()
    for symbol in obj.symbols.values():
        if symbol.storage in (2, 3) and symbol.section_index != 0:
            definitions.setdefault(symbol.name, []).append(symbol)
            if interface(obj, symbol):
                public.add(symbol.name)
    for symbol in obj.symbols.values():
        if symbol.storage != 105:
            continue
        _, mode = weak_target(obj, symbol)
        candidates = {
            (name, kind) for name, kind in evidence.get(symbol.name, ()) if name in definitions
        }
        if len(candidates) != 1:
            raise ValueError(f"Missing or ambiguous weak provenance: {symbol.name}")
        name, original_mode = next(iter(candidates))
        if original_mode != mode or len(definitions[name]) != 1:
            raise ValueError(f"Ambiguous or changed weak fallback: {symbol.name}")
        fallback = definitions[name][0]
        if fallback.section_index == -2:
            raise ValueError(f"Debug symbol is not a weak fallback: {symbol.name}")
        if fallback.section_index > 0:
            obj.address(fallback)
        if fallback.section_index > 0 and not interface(obj, fallback):
            # A COFF weak external cannot be local. Keep this resolved private
            # alias at its selected definition without reindexing records.
            struct.pack_into("<Ih", data, symbol.offset + 8, fallback.value, fallback.section_index)
            data[symbol.offset + 16] = 3
            data[symbol.offset + 18 : symbol.offset + 36] = bytes(18)
        else:
            if fallback.storage != 2:
                raise ValueError(f"Public weak fallback must be external: {symbol.name}")
            struct.pack_into("<I", data, symbol.offset + 18, fallback.index)
            public.update((symbol.name, fallback.name))
    fixed = CoffFile(data)
    private = {
        symbol.name
        for symbol in fixed.symbols.values()
        if symbol.storage == 2 and symbol.section_index != 0 and symbol.name not in public
    }
    return data, private


def localize(data, private):
    """Make the named external definitions static, as objcopy --localize-symbols does.

    Only storage classes and COMDAT flags change, so symbol indexes, auxiliary
    records and relocations stay valid. This needs no binutils objcopy, which LLVM toolchains
    do not have: llvm-objcopy cannot localize COFF symbols.
    """
    data = bytearray(data)
    for symbol in CoffFile(data).symbols.values():
        if symbol.name in private and symbol.storage == 2 and symbol.section_index != 0:
            data[symbol.offset + 16] = 3
    return separate_comdats(data)


def separate_comdats(data):
    """Keep private COMDAT sections out of the final link's COMDAT selection.

    The relocatable link already kept one copy of each COMDAT of the embedded
    software. A COMDAT whose key symbol is now static would still replace the
    runner's copies of the same COMDAT, such as Clang's .refptr stubs, and
    leave the runner's references to the key symbol undefined. Even merged
    correctly, such a copy can refer to the embedded software's private
    definitions. Clear the COMDAT flag of these sections and of the sections
    associated with them, so that each side keeps its own copy.
    """
    obj = CoffFile(data)
    data = bytearray(data)
    by_section = {}
    for symbol in sorted(obj.symbols.values(), key=lambda symbol: symbol.index):
        if symbol.section_index > 0:
            by_section.setdefault(symbol.section_index, []).append(symbol)
    separate, associative = set(), {}
    for index, section in obj.sections.items():
        if not section.flags & 0x1000:
            continue
        symbols = by_section.get(index, [])
        definition = next(
            (
                s
                for s in symbols
                if s.name == section.name and s.storage == 3 and s.aux_count == 1 and s.value == 0
            ),
            None,
        )
        if definition is None:
            raise ValueError(f"COMDAT section without a section definition: {section.name}")
        aux = definition.offset + 18
        number, selection = struct.unpack_from("<HB", data, aux + 12)
        if selection == 5:
            associative[index] = number
            continue
        key = next((s for s in symbols if s.index > definition.index), None)
        if key is None or key.storage != 2:
            separate.add(index)
    # Associations can chain. Follow them until nothing changes.
    changed = True
    while changed:
        changed = False
        for index, leader in associative.items():
            if leader in separate and index not in separate:
                separate.add(index)
                changed = True
    for index in separate:
        struct.pack_into(
            "<I", data, 20 + (index - 1) * 40 + 36, obj.sections[index].flags & ~0x1000
        )
    return data


def linker_sections(obj, image_path):
    """Give embedded sections PE page boundaries without changing input alignment."""
    groups = {"nsi_text": [], "nsi_ro": [], "nsi_data": [], "nsi_bss": []}
    filename = image_path.resolve().as_posix()
    if any(char in filename for char in '\n\r"'):
        raise ValueError("Unsupported embedded image path")
    for section in obj.sections.values():
        if section.size == 0 or not section.flags & 0xE0000000 or section.flags & 0x02000800:
            continue
        if any(char in section.name for char in '\n\r"()*?[]'):
            raise ValueError("Unsupported COFF section name in linker script")
        if section.flags & 0x20000000:
            if section.flags & 0x80000000:
                raise ValueError(f"Writable executable section is unsupported: {section.name}")
            group = "nsi_text"
        elif section.flags & 0x80:
            group = "nsi_bss"
        elif section.flags & 0x80000000:
            group = "nsi_data"
        else:
            group = "nsi_ro"
        entry = f'"{filename}"("{section.name}")'
        # Standard sections follow normal GC. The other sections were collected
        # by the embedded linker script (init/iterable tables and interfaces).
        if not section.name.startswith((".text", ".data", ".rdata", ".bss", ".eh_frame")):
            entry = f"KEEP({entry})"
        groups[group].append(entry)
    lines = ["SECTIONS {"]
    for name, entries in groups.items():
        if entries:
            lines.append(f"  .{name} BLOCK(__section_alignment__) : {{")
            lines.extend(f"    {entry}" for entry in entries)
            lines.append("  }")
    lines.append("} INSERT AFTER .data;\n")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--inputs", required=True, type=pathlib.Path)
    parser.add_argument("--input", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--linker-script", required=True, type=pathlib.Path)
    args = parser.parse_args()
    paths = [pathlib.Path(line) for line in args.inputs.read_text().splitlines() if line]
    outputs = {args.output.resolve(), args.linker_script.resolve()}
    originals = {path.resolve() for path in [args.input, args.inputs, *paths]}
    if len(outputs) != 2 or outputs & originals:
        raise ValueError("COFF output paths must be distinct from each other and from inputs")
    obj = CoffFile(args.input.read_bytes())
    data, private = repair(obj, provenance(paths))
    data = localize(data, private)
    with tempfile.TemporaryDirectory(dir=args.output.parent) as temp:
        temp = pathlib.Path(temp)
        output = temp / "localized.o"
        output.write_bytes(data)
        localized = CoffFile(data)
        for symbol in localized.symbols.values():
            if symbol.storage == 105:
                target, _ = weak_target(localized, symbol)
                if target not in localized.symbols or localized.symbols[target].storage != 2:
                    raise ValueError(f"Localization damaged weak fallback: {symbol.name}")
            if symbol.name in private and symbol.storage == 2:
                raise ValueError(f"Symbol was not localized: {symbol.name}")
        script = temp / "sections.ld"
        script.write_text(linker_sections(localized, args.output), encoding="utf-8")
        os.replace(script, args.linker_script)
        os.replace(output, args.output)


if __name__ == "__main__":
    main()
