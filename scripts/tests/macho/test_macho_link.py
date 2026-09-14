# SPDX-License-Identifier: Apache-2.0

import importlib.util
from pathlib import Path


SCRIPT = Path(__file__).parents[2] / "build" / "macho_link.py"
SPEC = importlib.util.spec_from_file_location("macho_link", SCRIPT)
MACHO_LINK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MACHO_LINK)


def test_input_files_unwraps_force_load(tmp_path):
    archive = tmp_path / "libzephyr.a"
    archive.touch()
    obj = tmp_path / "offsets.c.obj"
    obj.touch()

    command = [
        f"-Wl,-force_load,{archive}",
        str(obj),
        "@ignored-response-file",
        str(tmp_path / "missing.a"),
        "-o",
        "zephyr_pre0.elf",
    ]

    assert list(MACHO_LINK.input_files(command)) == [str(archive), str(obj)]


def test_registration_options_sort_and_localize(tmp_path):
    symbols = [
        ("__ZINIT", "__i4_100_0", "_late"),
        ("__ZINIT", "__i4_1_0", "_early"),
        ("__ZNSITASK", "__s2_20", "_task_late"),
        ("__ZNSITASK", "__s2_3", "_task_early"),
        ("__ZITER", "__k_sem", "_sem_z"),
        ("__ZITER", "__k_sem", "_sem_a"),
        ("absolute", "", "_CONFIG_TEST"),
    ]
    order_path = tmp_path / "order.txt"

    options = MACHO_LINK.registration_options(symbols, str(order_path))

    assert order_path.read_text().splitlines() == [
        "_early",
        "_late",
        "_sem_a",
        "_sem_z",
        "_task_early",
        "_task_late",
    ]
    assert (tmp_path / "order.txt.unexported").read_text() == "_CONFIG_TEST\n"
    assert f"-Wl,-order_file,{order_path}" in options
    assert "-Wl,-rename_section,__ZINIT,__i4_1_0,__ZINIT,__i4" in options
    assert "-Wl,-rename_section,__ZINIT,__i4_100_0,__ZINIT,__i4" in options


def test_partial_link_preserves_priority_section_names(tmp_path):
    symbols = [('__ZNSIEVT', '__e_999', '_boundary'), ('__ZNSIEVT', '__e_0', '_timer')]
    order_path = tmp_path / 'partial.txt'
    assert MACHO_LINK.registration_options(symbols, str(order_path), relocatable=True) == []
    assert not order_path.exists()


def test_equal_priorities_keep_input_order(tmp_path):
    symbols = [('__ZNSIEVT', '__e_999', '_z'), ('__ZNSIEVT', '__e_999', '_a'),
               ('__ZNSIEVT', '__e_0', '_timer')]
    order_path = tmp_path / 'ties.txt'
    MACHO_LINK.registration_options(symbols, str(order_path))
    assert order_path.read_text().splitlines() == ['_timer', '_z', '_a']


def test_registration_options_without_metadata(tmp_path):
    order_path = tmp_path / "unused.txt"

    assert MACHO_LINK.registration_options([], str(order_path)) == []
    assert not order_path.exists()
