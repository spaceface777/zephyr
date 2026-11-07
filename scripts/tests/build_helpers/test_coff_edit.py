# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Rewriting i386 COFF objects and archives for LLD's COFF layout."""

import os
import struct
import sys

sys.path.insert(0, os.path.join(os.environ["ZEPHYR_BASE"], "scripts/pylib/build_helpers"))
import coff_edit as ce  # noqa: E402
from coff_parser import CoffFile  # noqa: E402

DATA = ce.SCN_CNT_INITIALIZED_DATA | ce.SCN_MEM_READ | ce.SCN_MEM_WRITE
BSS = ce.SCN_CNT_UNINITIALIZED_DATA | ce.SCN_MEM_READ | ce.SCN_MEM_WRITE


def sample():
    relocation = struct.pack("<IIH", 0, 2, 6)  # a DIR32 at offset 0 against symbol 2
    sections = [
        ce.Section(".data", ce.with_alignment(DATA, 4), 8, bytes(range(8)), relocation, b"", 0),
        ce.Section(".bss", ce.with_alignment(BSS, 8), 16, None, b"", b"", 0),
    ]
    aux = struct.pack("<IHHIHBB", 8, 1, 0, 0, 0, 0, 0) + bytes(2)
    symbols = [
        ce.Symbol(".data", 0, 1, 0, ce.SYM_CLASS_STATIC, aux),
        ce.Symbol("_a_rather_long_definition", 4, 1, 0, ce.SYM_CLASS_EXTERNAL, b""),
        ce.Symbol("_extern", 0, 0, 0, ce.SYM_CLASS_EXTERNAL, b""),
        ce.Symbol("_common", 16, 0, 0, ce.SYM_CLASS_EXTERNAL, b""),
    ]
    return ce.write_object(sections, symbols)


def test_round_trip_is_identical_and_readable():
    data = sample()
    assert ce.CoffObject(data).to_bytes() == data
    parsed = CoffFile(data)
    assert [section.name for section in parsed.sections.values()] == [".data", ".bss"]
    names = [symbol.name for symbol in parsed.symbols.values()]
    assert names == [".data", "_a_rather_long_definition", "_extern", "_common"]


def test_renames_use_the_string_table():
    obj = ce.CoffObject(sample())
    obj.rename_section(0, ".data$~e0000010000002.z_init_EARLY")
    obj.symbols[3].name = "__nsi_esw._extern"  # index 1 is the section symbol's auxiliary record
    obj.make_initialized(1)
    parsed = CoffFile(obj.to_bytes())
    assert parsed.sections[1].name == ".data$~e0000010000002.z_init_EARLY"
    # The section definition symbol follows its section; relocations keep their target index.
    assert parsed.symbols[0].name == ".data$~e0000010000002.z_init_EARLY"
    assert parsed.symbols[3].name == "__nsi_esw._extern"
    assert parsed.sections[2].flags & ce.SCN_CNT_INITIALIZED_DATA
    reread = ce.CoffObject(obj.to_bytes())
    assert reread.sections[1].data == bytes(16)
    assert reread.sections[0].relocations == struct.pack("<IIH", 0, 2, 6)


def test_definitions_include_commons_and_exclude_references():
    obj = ce.CoffObject(sample())
    assert sorted(symbol.name for symbol in obj.defined_symbols()) == [
        "_a_rather_long_definition",
        "_common",
    ]


def test_archives_keep_order_names_and_index():
    members = [
        ("short.o", sample()),
        ("a_very_long_member_name.c.obj", sample()),
        ("short.o", sample()),
    ]
    archive = ce.write_archive(members)
    assert ce.read_archive(archive) == members
    count = struct.unpack_from(">I", archive, 8 + 60)[0]
    assert count == 6  # two definitions in each of three members


def test_reads_llvm_ar_coff_long_names():
    names = b"a_very_long_member_name.c.obj\0"
    header = b"//".ljust(16) + b"0".ljust(32) + str(len(names)).encode().ljust(10) + b"`\n"
    member = b"/0".ljust(16) + b"0".ljust(32) + b"2".ljust(10) + b"`\nxy"
    assert ce.read_archive(b"!<arch>\n" + header + names + member) == [
        ("a_very_long_member_name.c.obj", b"xy")
    ]


def test_alignment_flags():
    assert ce.alignment(ce.with_alignment(DATA, 1)) == 1
    assert ce.alignment(ce.with_alignment(DATA, 4096)) == 4096
    assert ce.alignment(DATA) == 16
