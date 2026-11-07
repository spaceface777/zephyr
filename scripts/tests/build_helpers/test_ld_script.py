# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""The linker script subset that LLD's COFF layout reads from Zephyr's generated scripts."""

import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.environ["ZEPHYR_BASE"], "scripts/pylib/build_helpers"))
import ld_script as ld  # noqa: E402

SCRIPT = """
/* comment */
SECTIONS
{
 initlevel :
 {
  __init_start = .; ___init_start = __init_start;
  KEEP(*(SORT(.z_init_EARLY_P_?_*))); KEEP(*(SORT(.z_init_EARLY_P_??_*)));
  __init_end = .;
 }
 ASSERT(SIZEOF(initlevel) == 0, "Undefined initialization levels used.")
 device_api_area : SUBALIGN(8) { *(SORT_BY_NAME(._api.static.*)) }
 init_array :
 {
  KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
  KEEP (*(.init_array .ctors))
  LONG(0)
 }
 states : { . = ALIGN(4); KEEP(*(".z_devstate")); }
 __data_region_end = .;
 nsi_tasks BLOCK(__section_alignment__) : { PROVIDE(__start = .); } > RAM
 /DISCARD/ : { *(.note.GNU-stack) }
} INSERT AFTER .data;
"""


def test_parses_zephyr_constructs():
    commands = ld.parse(SCRIPT)
    assert len(commands) == 1
    assert commands[0].insert == ("AFTER", ".data")
    items = commands[0].items
    names = [item.name for item in items if isinstance(item, ld.OutputSection)]
    assert names == [
        "initlevel",
        "device_api_area",
        "init_array",
        "states",
        "nsi_tasks",
        "/DISCARD/",
    ]

    initlevel = items[0]
    assert [type(item) for item in initlevel.items] == [
        ld.Assign,
        ld.Assign,
        ld.Input,
        ld.Input,
        ld.Assign,
    ]
    assert initlevel.items[1].expr == ld.Expr("symbol", "__init_start")
    assert initlevel.items[2].keep and initlevel.items[2].specs[0].sort == ("SORT",)

    check = items[1]
    assert isinstance(check, ld.Assert) and check.message == "Undefined initialization levels used."
    assert check.expr.kind == "binary" and check.expr.left == ld.Expr("sizeof", "initlevel")

    assert ld.constant(items[2].subalign) == 8
    sorted_specs = items[3].items[0].specs
    assert [spec.sort for spec in sorted_specs] == [("SORT_BY_INIT_PRIORITY",)] * 2
    assert items[3].items[1].specs[0].patterns == [".init_array", ".ctors"]
    assert isinstance(items[3].items[2], ld.Data) and items[3].items[2].size == 4

    assert isinstance(items[4].items[0], ld.Dot)
    assert items[4].items[1].specs[0].patterns == [".z_devstate"]
    assert isinstance(items[5], ld.Assign) and items[5].name == "__data_region_end"
    assert items[6].items[0].provide
    assert items[7].discard


@pytest.mark.parametrize(
    "text",
    [
        "MEMORY { RAM : ORIGIN = 0, LENGTH = 1K }",
        "SECTIONS { out : { *(.a) } } INSERT INTO .data;",
        "SECTIONS { out (WEIRD) : { *(.a) } }",
        "SECTIONS { out : { libfoo.a:*(.a) EXCLUDE_FILE(*crt0.o) } }",
    ],
)
def test_rejects_what_it_does_not_model(text):
    with pytest.raises(ld.ScriptError):
        ld.parse(text)


def test_matching_and_init_priority():
    spec = ld.SectionSpec([".native_PRE_BOOT_1[0-9]_task", ".z_init_*"])
    assert ld.matches(spec, ".native_PRE_BOOT_15_task")
    assert not ld.matches(spec, ".native_PRE_BOOT_115_task")
    assert ld.matches(spec, ".z_init_EARLY_P_1_SUB_0_")
    assert ld.init_priority(".init_array.00100") == 100
    assert ld.init_priority(".ctors.65435") == 100
    assert ld.init_priority(".init_array") == 65536
