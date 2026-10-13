# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Tests for Mach-O device API hierarchy layout and bounds."""

import struct

import pytest
from gen_macho_aliases import bound_target
from gen_macho_iter_sections import shorten
from order_macho_sections import object_metadata, order_sections

PARENTS = {"base": ["child", "sibling"], "child": ["grandchild"]}
CHILDREN = {"child", "grandchild", "sibling"}
NAMES = ["extra", "sibling", "base", "gap", "grandchild", "child", "ordinary"]


def make_object(cpu=0x100000C):
    """Create an object with symbols and both local and external relocations."""
    count = len(NAMES)
    segment_size = 72 + 80 * count
    commands_size = segment_size + 24
    payload = 32 + commands_size
    relocations = payload + count * 8
    symtab = relocations + 24
    strings = symtab + count * 16
    data = bytearray(struct.pack("<IiiIIIII", 0xFEEDFACF, cpu, 0, 1, 2, commands_size, 0, 0))
    data.extend(
        struct.pack(
            "<II16sQQQQiiII",
            0x19,
            segment_size,
            b"",
            0,
            count * 8,
            payload,
            count * 8,
            7,
            7,
            count,
            0,
        )
    )
    for index, name in enumerate(NAMES):
        data.extend(
            struct.pack(
                "<16s16sQQIIIIIIII",
                shorten(name).encode(),
                b"__DATA",
                index * 8,
                8,
                payload + index * 8,
                3,
                relocations if index == 0 else 0,
                3 if index == 0 else 0,
                0,
                0,
                0,
                0,
            )
        )
    data.extend(struct.pack("<IIIIII", 2, 24, symtab, count, strings, 7))
    data.extend(bytes(range(count * 8)))
    addend = (10 << 28) | 0xABCDEF if cpu == 0x100000C else 3
    data.extend(struct.pack("<IIIIII", 0, 3, 0, (1 << 27) | 1, 0, addend))
    for index in range(count):
        data.extend(struct.pack("<IBBHQ", 1, 0xE, index + 1, 0, index * 8))
    data.extend(b"\0entry\0")
    return bytes(data), payload, relocations, symtab


@pytest.mark.parametrize("cpu", [0x100000C, 0x1000007])
def test_hierarchy_sections_and_references(cpu):
    data, payload, relocations, symtab = make_object(cpu)
    ordered = order_sections(data, PARENTS, CHILDREN)
    _, _, sections, _ = object_metadata(ordered)
    expected = ["extra", "base", "child", "grandchild", "sibling", "gap", "ordinary"]
    assert [s[:16].split(b"\0")[0].decode() for s in sections] == [shorten(n) for n in expected]
    assert ordered[payload:relocations] == data[payload:relocations]
    assert struct.unpack_from("<I", ordered, relocations + 4)[0] == 2
    assert ordered[relocations + 8 : relocations + 16] == data[relocations + 8 : relocations + 16]
    if cpu == 0x100000C:
        assert (
            ordered[relocations + 16 : relocations + 24]
            == data[relocations + 16 : relocations + 24]
        )
    else:
        assert struct.unpack_from("<I", ordered, relocations + 20)[0] == 2
    for index, name in enumerate(NAMES):
        assert ordered[symtab + index * 16 + 5] == expected.index(name) + 1
    assert order_sections(ordered, PARENTS, CHILDREN) == ordered


def test_no_hierarchy_is_unchanged():
    data, _, _, _ = make_object()
    assert order_sections(data, {}, set()) == data


@pytest.mark.parametrize(
    "name,bound,target,kind",
    [
        ("base", "list_start", "base", "start"),
        ("base", "list_end", "base", "end"),
        ("base", "ext_end", "sibling", "end"),
        ("child", "ext_end", "grandchild", "end"),
        ("sibling", "ext_end", "sibling", "end"),
        ("ordinary", "ext_end", "ordinary", "end"),
    ],
)
def test_bounds_include_only_descendants(name, bound, target, kind):
    mapping = {n: shorten(n) for n in NAMES}
    assert bound_target(name, bound, mapping, PARENTS, set(mapping.values())) == (
        f"section${kind}$__DATA${mapping[target]}"
    )


@pytest.mark.parametrize("bound", ["list_start", "list_end"])
def test_parent_without_instances_starts_at_first_descendant(bound):
    mapping = {n: shorten(n) for n in NAMES}
    present = {mapping["child"], mapping["grandchild"]}
    assert bound_target("base", bound, mapping, PARENTS, present) == (
        f"section$start$__DATA${mapping['child']}"
    )
    assert bound_target("base", "ext_end", mapping, PARENTS, present) == (
        f"section$end$__DATA${mapping['grandchild']}"
    )


def test_absent_hierarchy_has_empty_own_bounds():
    mapping = {n: shorten(n) for n in NAMES}
    assert bound_target("base", "ext_end", mapping, PARENTS, set()) == (
        f"section$end$__DATA${mapping['base']}"
    )


@pytest.mark.parametrize("offset,value", [(0, 0), (12, 2), (20, 0xFFFFFFFF)])
def test_invalid_object_header(offset, value):
    data = bytearray(make_object()[0])
    struct.pack_into("<I", data, offset, value)
    with pytest.raises(ValueError):
        order_sections(data, PARENTS, CHILDREN)


def test_truncated_object():
    with pytest.raises(ValueError):
        order_sections(b"", PARENTS, CHILDREN)


def test_invalid_local_relocation():
    original, _, relocations, _ = make_object()
    data = bytearray(original)
    struct.pack_into("<I", data, relocations + 4, 255)
    with pytest.raises(ValueError, match="relocation has an invalid section index"):
        order_sections(data, PARENTS, CHILDREN)


def test_invalid_symbol_section():
    original, _, _, symtab = make_object()
    data = bytearray(original)
    data[symtab + 5] = 255
    with pytest.raises(ValueError, match="symbol has an invalid section index"):
        order_sections(data, PARENTS, CHILDREN)


def test_scattered_relocation_is_rejected():
    original, _, relocations, _ = make_object()
    data = bytearray(original)
    struct.pack_into("<I", data, relocations, 0x80000000)
    with pytest.raises(ValueError, match="Scattered relocations"):
        order_sections(data, PARENTS, CHILDREN)
