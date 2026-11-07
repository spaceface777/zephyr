# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Localization of the Windows native simulator's embedded COFF image."""

import os
import struct
import sys

import pytest

sys.path.insert(0, os.path.join(os.environ["ZEPHYR_BASE"], "scripts/build"))
sys.path.insert(0, os.path.join(os.environ["ZEPHYR_BASE"], "scripts/pylib/build_helpers"))
import localize_coff  # noqa: E402
from coff_parser import CoffFile  # noqa: E402

TEXT = 0x60000020
RDATA = 0x40000040
COMDAT = 0x1000


def coff_object(sections, symbols):
    """Build an i386 COFF object.

    sections: (name, flags, size) tuples, numbered from 1.
    symbols: (name, value, section, storage, aux) tuples; aux is 18 bytes or None.
    """
    data_offset = 20 + 40 * len(sections)
    headers, contents = b"", b""
    for name, flags, size in sections:
        offset = data_offset + len(contents) if size else 0
        headers += struct.pack("<8sIIIIIIHHI", name.encode(), 0, 0, size, offset, 0, 0, 0, 0, flags)
        contents += bytes(size)
    table, count = b"", 0
    for name, value, section, storage, aux in symbols:
        table += struct.pack("<8sIhHBB", name.encode(), value, section, 0, storage, int(bool(aux)))
        count += 1
        if aux:
            table += aux
            count += 1
    header = struct.pack(
        "<HHIIIHH", 0x14C, len(sections), 0, data_offset + len(contents), count, 0, 0
    )
    return header + headers + contents + table + struct.pack("<I", 4)


def section_definition(size, number=0, selection=0):
    return struct.pack("<IHHIHB3x", size, 0, 0, 0, number, selection)


def weak_external(target, mode):
    return struct.pack("<II10x", target, mode)


def by_name(obj):
    return {symbol.name: symbol for symbol in obj.symbols.values()}


def test_localize_makes_only_the_private_definitions_static():
    data = coff_object(
        [(".text", TEXT, 8)],
        [
            ("_private", 0, 1, 2, None),
            ("_public", 4, 1, 2, None),
            ("_host", 0, 0, 2, None),
        ],
    )
    symbols = by_name(CoffFile(localize_coff.localize(data, {"_private", "_host"})))
    assert symbols["_private"].storage == 3
    assert symbols["_public"].storage == 2
    # An undefined reference stays external, so the host can still resolve it.
    assert symbols["_host"].storage == 2


def test_private_comdats_and_their_associates_stop_being_comdats():
    data = coff_object(
        [
            (".rdata$p", RDATA | COMDAT, 4),
            (".xdata$p", RDATA | COMDAT, 4),
            (".rdata$q", RDATA | COMDAT, 4),
        ],
        [
            (".rdata$p", 0, 1, 3, section_definition(4, selection=2)),
            ("_ptr", 0, 1, 2, None),
            (".xdata$p", 0, 2, 3, section_definition(4, number=1, selection=5)),
            (".rdata$q", 0, 3, 3, section_definition(4, selection=2)),
            ("_shared", 0, 3, 2, None),
        ],
    )
    obj = CoffFile(localize_coff.localize(data, {"_ptr"}))
    # The runner's copies of a private COMDAT must not be replaced by this one.
    assert not obj.sections[1].flags & COMDAT
    assert not obj.sections[2].flags & COMDAT
    assert obj.sections[1].flags == RDATA
    # A COMDAT whose key symbol stays public is still merged with other copies.
    assert obj.sections[3].flags & COMDAT
    assert by_name(obj)["_ptr"].storage == 3


def test_a_comdat_needs_its_section_definition():
    data = coff_object([(".rdata$p", RDATA | COMDAT, 4)], [("_ptr", 0, 1, 2, None)])
    with pytest.raises(ValueError, match="without a section definition"):
        localize_coff.localize(data, {"_ptr"})


@pytest.mark.parametrize("mode", [1, 3])
def test_weak_externals_of_gcc_and_clang(mode):
    data = coff_object(
        [(".text", TEXT, 4)],
        [("_dflt", 0, 1, 2, None), ("_weak", 0, 0, 105, weak_external(0, mode))],
    )
    obj = CoffFile(data)
    assert localize_coff.weak_target(obj, by_name(obj)["_weak"]) == (0, mode)


def test_weak_externals_that_search_libraries_are_rejected():
    data = coff_object(
        [(".text", TEXT, 4)],
        [("_dflt", 0, 1, 2, None), ("_weak", 0, 0, 105, weak_external(0, 2))],
    )
    obj = CoffFile(data)
    with pytest.raises(ValueError, match="search mode"):
        localize_coff.weak_target(obj, by_name(obj)["_weak"])


def archive(names_table, members):
    def member(name, data):
        header = f"{name:<16}{0:<12}{0:<6}{0:<6}{644:<8}{len(data):<10}`\n".encode()
        return header + data + (b"\n" if len(data) & 1 else b"")

    body = member("//", names_table)
    for name, data in members:
        body += member(name, data)
    return b"!<arch>\n" + body


@pytest.mark.parametrize(
    "names_table",
    [
        b"first_long_name.obj/\nsecond_long_name.obj/\n",
        b"first_long_name.obj\0second_long_name.obj\0",
    ],
    ids=["gnu", "llvm-ar-coff"],
)
def test_archive_long_names(names_table):
    second = (
        len("first_long_name.obj") + 2 if b"/\n" in names_table else len("first_long_name.obj") + 1
    )
    data = archive(names_table, [("/0", b"one"), (f"/{second}", b"two!"), ("short.obj/", b"3")])
    members = [
        (name.rsplit("@", 1)[0], content) for name, content in localize_coff.archive_members(data)
    ]
    assert members == [
        ("first_long_name.obj", b"one"),
        ("second_long_name.obj", b"two!"),
        ("short.obj", b"3"),
    ]
