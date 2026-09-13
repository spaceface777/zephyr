# SPDX-License-Identifier: Apache-2.0

import importlib.util
from pathlib import Path
from types import SimpleNamespace


SCRIPT = Path(__file__).parents[2] / "build" / "gen_offset_header.py"
SPEC = importlib.util.spec_from_file_location("gen_offset_header", SCRIPT)
GEN_OFFSET = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GEN_OFFSET)


def test_macho_absolute_symbols(monkeypatch):
    result = SimpleNamespace(
        stdout="_TEST_OFFSET A 2a 0\n_THING_SIZEOF A 18 0\n_local t 0 0\n"
    )
    monkeypatch.setattr(GEN_OFFSET.subprocess, "run", lambda *args, **kwargs: result)

    assert list(GEN_OFFSET.macho_symbols("input.o", "nm")) == [
        ("TEST_OFFSET", 0x2A),
        ("THING_SIZEOF", 0x18),
    ]
