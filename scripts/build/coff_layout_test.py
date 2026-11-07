#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Linker script layout and localization of COFF inputs for LLD."""

import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import coff_layout as cl  # noqa: E402
from coff_layout import ce  # noqa: E402

RDATA = ce.SCN_CNT_INITIALIZED_DATA | ce.SCN_MEM_READ
DATA = RDATA | ce.SCN_MEM_WRITE
BSS = ce.SCN_CNT_UNINITIALIZED_DATA | ce.SCN_MEM_READ | ce.SCN_MEM_WRITE
TEXT = ce.SCN_CNT_CODE | ce.SCN_MEM_EXECUTE | ce.SCN_MEM_READ

SCRIPT = """
SECTIONS {
 initlevel : {
  __init_start = .; ___init_start = __init_start;
  KEEP(*(SORT(.z_init_P_?_*))); KEEP(*(SORT(.z_init_P_??_*)));
  __init_end = .;
 }
 states : { . = ALIGN(4); __states_start = .; KEEP(*(.z_devstate)); __states_end = .; LONG(7) }
 ASSERT(__init_start != __init_end, "no init entries")
 __region_end = .;
 noinit : { *(.noinit_explicit) }
} INSERT AFTER .data;
"""


def unit(sections, symbols=()):
    obj = ce.CoffObject(ce.write_object(sections, list(symbols)))
    return cl.Unit(0, None, obj)


def section(name, flags, size=8, align=4):
    data = None if flags & ce.SCN_CNT_UNINITIALIZED_DATA else bytes(size)
    return ce.Section(name, ce.with_alignment(flags, align), size, data, b"", b"", 0)


def external(name, number, value=0):
    return ce.Symbol(name, value, number, 0, ce.SYM_CLASS_EXTERNAL, b"")


def test_sorts_like_gnu_ld_and_defines_markers():
    first = unit([section(".z_init_P_10_SUB_0_", RDATA), section(".z_init_P_2_SUB_0_", RDATA)])
    second = unit(
        [
            section(".z_init_P_5_SUB_1_", RDATA),
            section(".z_devstate", BSS, 2, 2),
            section(".noinit.7", BSS),
        ]
    )
    layout = cl.Layout([SCRIPT], "e")
    layout.apply([first, second])
    # LLD orders the sections of one output section by name.
    order = sorted(
        s.name for u in (first, second) for s in u.obj.sections if s.name.startswith(".rdata$")
    )
    originals = [layout.renamed[name] for name in order]
    # Priority 2 and 5 sort by name before the two-digit priority 10, as in GNU ld.
    assert originals == [".z_init_P_2_SUB_0_", ".z_init_P_5_SUB_1_", ".z_init_P_10_SUB_0_"]
    assert all(name.startswith(".rdata$~e") for name in order)
    # The data statement makes the states section initialized data, including its BSS input.
    devstate = second.obj.sections[1]
    assert devstate.name.startswith(".data$~e") and devstate.data == bytes(2)
    # Unplaced sections join the standard section for their contents.
    assert second.obj.sections[2].name == ".bss$~~.noinit.7"

    markers = ce.CoffObject(layout.markers())
    labels = {
        symbol.name: (markers.sections[symbol.section - 1].name, symbol.value)
        for symbol in markers.symbols
        if symbol is not None
    }
    assert labels["___init_start"] == labels["__init_start"]
    assert labels["__init_start"][0] < order[0] < order[-1] < labels["__init_end"][0]
    assert ce.alignment(markers.sections[0].flags) == 4  # the alignment of the entries
    assert labels["__region_end"][0] > labels["__states_end"][0]
    data_marker = next(s for s in markers.sections if s.data and any(s.data))
    assert data_marker.data == struct.pack("<I", 7)


def test_failed_assertion_stops_the_link():
    layout = cl.Layout([SCRIPT], "e")
    with pytest.raises(cl.LayoutError, match="no init entries"):
        layout.apply([unit([section(".data", DATA)])])


def test_code_and_data_in_one_output_section_are_rejected():
    script = "SECTIONS { mixed : { *(.mine.*) } }"
    with pytest.raises(cl.LayoutError, match="mixes code and data"):
        cl.Layout([script], "e").apply([unit([section(".mine.a", TEXT), section(".mine.b", DATA)])])


def test_namespaces_keep_layouts_apart():
    script = "SECTIONS { tasks : { KEEP(*(SORT(.task_*))) } }"
    embedded, runner = unit([section(".task_1", RDATA)]), unit([section(".task_1", RDATA)])
    cl.Layout([script], "e").apply([embedded])
    cl.Layout([script], "r").apply([runner])
    assert embedded.obj.sections[0].name.startswith(".rdata$~e")
    assert runner.obj.sections[0].name.startswith(".rdata$~r")


def test_localization_keeps_interfaces_public():
    weak_aux = struct.pack("<II", 0, 1) + bytes(10)  # falls back to _nsif_boot
    interface = unit(
        [section(".native_sim_if", TEXT), section(".text", TEXT)],
        [
            external("_nsif_boot", 1),
            external("_private", 2),
            external("_host_puts", 0),
            ce.Symbol("_hook", 0, 0, 0, ce.SYM_CLASS_WEAK_EXTERNAL, weak_aux),
        ],
    )
    caller = unit(
        [section(".text", TEXT)], [external("_private", 0), external("__imp__private", 0)]
    )
    names = cl.localize([interface, caller], lambda name: name.startswith(".native_sim_if"), "P.")
    renamed = [s.name for u in (interface, caller) for s in u.obj.symbols if s is not None]
    assert renamed == [
        "_nsif_boot",
        "P._private",
        "_host_puts",
        "_hook",
        "P._private",
        "__imp_P._private",
    ]
    assert names["_private"] == "P._private"


def test_link_command_parsing():
    compiler, arguments = cl.split_compiler(
        ["ccache.exe", "clang.exe", "--target=i686-w64-windows-gnu", "-Oz", "a.obj"]
    )
    assert compiler == ["ccache.exe", "clang.exe"]
    link = cl.parse_link_command(
        arguments
        + [
            "-Wl,-T",
            "linker.cmd",
            "-Wl,--whole-archive",
            "z.a",
            "-Wl,--no-whole-archive",
            "k.a",
            "-Wl,-u,_Abs",
            "-Wl,-Map=out.map",
            "-r",
            "-lgcc",
            "-Wl,--build-id=none",
        ]
    )
    assert link["scripts"] == ["linker.cmd"]
    assert link["inputs"] == [("a.obj", False), ("z.a", True), ("k.a", False)]
    assert link["undefined"] == ["_Abs"] and link["map"] == "out.map"
    assert link["driver"] == ["--target=i686-w64-windows-gnu", "-Oz"]
    assert cl.split_windows(r'a "b c" dir\file.obj "q\"x"') == ["a", "b c", r"dir\file.obj", 'q"x']


def test_alignment_after_a_label_starts_an_aligned_marker():
    script = "SECTIONS { s : { __a = .; LONG(1) . = ALIGN(16); __b = .; *(.x) } }"
    layout = cl.Layout([script], "e")
    layout.apply([unit([section(".x", DATA, 4, 4)])])
    markers = ce.CoffObject(layout.markers())
    assert [ce.alignment(s.flags) for s in markers.sections] == [4, 16]
    assert layout.labels["__a"] == ("s", 0) and layout.labels["__b"] == ("s", 16)
