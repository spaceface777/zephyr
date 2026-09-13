#!/usr/bin/env python3
#
# Copyright (c) 2017 Intel Corporation.
#
# SPDX-License-Identifier: Apache-2.0
#

"""
This script scans a specified object file and generates a header file
that defined macros for the offsets of various found structure members
(particularly symbols ending with ``_OFFSET`` or ``_SIZEOF``), primarily
intended for use in assembly code.
"""

import argparse
import os
import subprocess
import sys

from elftools.elf.elffile import ELFFile
from elftools.elf.sections import SymbolTableSection


MACHO_MAGICS = {
    b"\xce\xfa\xed\xfe",
    b"\xcf\xfa\xed\xfe",
    b"\xfe\xed\xfa\xce",
    b"\xfe\xed\xfa\xcf",
}


def get_symbol_table(obj):
    for section in obj.iter_sections():
        if isinstance(section, SymbolTableSection):
            return section

    raise LookupError("Could not find symbol table")


def elf_symbols(input_file):
    obj = ELFFile(input_file)
    for sym in get_symbol_table(obj).iter_symbols():
        name = sym.name.decode("ascii") if isinstance(sym.name, bytes) else sym.name
        if sym.entry["st_shndx"] != "SHN_ABS":
            continue
        if sym.entry["st_info"]["bind"] != "STB_GLOBAL":
            continue
        yield name, sym.entry["st_value"]


def macho_symbols(input_name, nm):
    result = subprocess.run(
        [nm, "-P", input_name],
        check=True,
        stdout=subprocess.PIPE,
        text=True,
    )
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) < 3 or fields[1] != "A":
            continue
        name = fields[0]
        if name.startswith("_"):
            name = name[1:]
        yield name, int(fields[2], 16)


def gen_offset_header(input_file, output_file, input_name=None, nm="nm"):
    basename = os.path.basename(output_file.name).upper().replace('.', '_').replace('-', '_')
    include_guard = f"__GEN_{basename}__"
    output_file.write(
        f"""/* THIS FILE IS AUTO GENERATED.  PLEASE DO NOT EDIT.
 *
 * This header file provides macros for the offsets of various structure
 * members.  These offset macros are primarily intended to be used in
 * assembly code.
 */

#ifndef {include_guard}
#define {include_guard}\n\n"""
    )

    magic = input_file.read(4)
    input_file.seek(0)
    if magic in MACHO_MAGICS:
        if input_name is None:
            raise ValueError("Mach-O input requires a file name")
        symbols = macho_symbols(input_name, nm)
    else:
        symbols = elf_symbols(input_file)

    seen = set()
    for name, value in symbols:
        if not name.endswith(("_OFFSET", "_SIZEOF")):
            continue
        if name in seen:
            raise ValueError(f"Duplicate absolute symbol: {name}")
        seen.add(name)
        output_file.write(f"#define {name} 0x{value:x}\n")

    output_file.write(f"\n#endif /* {include_guard} */\n")

    return 0


if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        allow_abbrev=False,
    )

    parser.add_argument("-i", "--input", required=True, help="Input object file")
    parser.add_argument("-o", "--output", required=True, help="Output header file")
    parser.add_argument("--nm", default="nm", help="nm executable for Mach-O input")

    args = parser.parse_args()

    with open(args.input, 'rb') as input_file, open(args.output, 'w') as output_file:
        ret = gen_offset_header(input_file, output_file, args.input, args.nm)

    sys.exit(ret)
