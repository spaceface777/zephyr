# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Relocation-count boundaries for the native simulator's i386 COFF reader."""

import os
import struct
import sys

import pytest

sys.path.insert(0, os.path.join(os.environ["ZEPHYR_BASE"], "scripts/pylib/build_helpers"))
from coff_parser import CoffFile  # noqa: E402


def coff_object(count, extended=False):
    """One section, one symbol, and repeated valid relocations to that symbol."""
    relocations = struct.pack("<IIH", 0, 0, 6) * count
    if extended:
        relocations = struct.pack("<IIH", count + 1, 0, 0) + relocations
    header = struct.pack("<HHIIIHH", 0x14C, 1, 0, 64 + len(relocations), 1, 0, 0)
    section = struct.pack(
        "<8sIIIIIIHHI",
        b".data",
        0,
        0,
        4,
        60,
        64,
        0,
        0xFFFF if extended else count,
        0,
        0x40000040 | (0x01000000 if extended else 0),
    )
    symbol = struct.pack("<8sIhHBB", b"value", 0, 1, 0, 2, 0)
    return bytearray(header + section + bytes(4) + relocations + symbol + struct.pack("<I", 4))


@pytest.mark.parametrize("count", [0, 1, 0xFFFF])
def test_ordinary_relocations(count):
    assert CoffFile(coff_object(count)).symbols[0].name == "value"


@pytest.mark.parametrize("count", [0xFFFF, 0x10000])
def test_extended_relocations(count):
    assert CoffFile(coff_object(count, extended=True)).symbols[0].name == "value"


@pytest.mark.parametrize("count,offset", [(0, 64), (1, 64), (0xFFFF, 0)])
def test_invalid_overflow_header(count, offset):
    data = coff_object(0xFFFF, extended=True)
    struct.pack_into("<I", data, 44, offset)
    struct.pack_into("<H", data, 52, count)
    with pytest.raises(ValueError, match="overflow header"):
        CoffFile(data)


@pytest.mark.parametrize("record", [(0xFFFF, 0, 0), (0x10000, 1, 0), (0x10000, 0, 6)])
def test_invalid_overflow_sentinel(record):
    data = coff_object(0xFFFF, extended=True)
    struct.pack_into("<IIH", data, 64, *record)
    with pytest.raises(ValueError, match="overflow count"):
        CoffFile(data)


def test_extended_count_cannot_exceed_file():
    data = coff_object(0xFFFF, extended=True)
    struct.pack_into("<I", data, 64, 0x10003)
    with pytest.raises(ValueError, match="beyond the file"):
        CoffFile(data)


def test_validates_relocations_after_the_16_bit_limit():
    data = coff_object(0x10000, extended=True)
    struct.pack_into("<IIH", data, 64 + 0x10000 * 10, 0, 1, 6)
    with pytest.raises(ValueError, match="relocation target"):
        CoffFile(data)
