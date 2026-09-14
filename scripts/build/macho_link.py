#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Add Mach-O registration ordering to a compiler-driver link command."""

import argparse
import os
import re
import subprocess
import sys
import tempfile


REGISTRATION = {
    "__ZINIT": (re.compile(r"__i(\d+)_(\d+)_(\d+)$"), "__i{}"),
    "__ZNATIVE": (re.compile(r"__n(\d+)_(\d+)$"), "__n{}"),
    "__ZNSITASK": (re.compile(r"__s(\d+)_(\d+)$"), "__s{}"),
    "__ZNSIEVT": (re.compile(r"__e_(\d+)$"), "__events"),
}
NM_LINE = re.compile(r"^[0-9a-fA-F]+ \(([^,]+),([^\)]+)\) .* ([^ ]+)$")
NM_ABSOLUTE = re.compile(r"^[0-9a-fA-F]+ \(absolute\) external .* ([^ ]+)$")


def input_files(command):
    force_load_prefix = "-Wl,-force_load,"
    for arg in command:
        if arg.startswith("@"):
            continue
        if arg.startswith(force_load_prefix):
            arg = arg[len(force_load_prefix):]
        if os.path.isfile(arg) and arg.endswith((".a", ".o", ".obj")):
            yield arg


def read_symbols(nm, paths):
    found = []
    for path in paths:
        result = subprocess.run(
            [nm, "-m", path], check=True, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True,
        )
        for line in result.stdout.splitlines():
            match = NM_LINE.match(line)
            if match:
                found.append((match.group(1), match.group(2), match.group(3)))
                continue
            match = NM_ABSOLUTE.match(line)
            if match:
                found.append(("absolute", "", match.group(1)))
    return found


def registration_options(symbols, order_path, relocatable=False):
    order = []
    renames = set()
    iterable = {}
    absolute = set()

    for segment, section, symbol in symbols:
        if symbol.startswith("ltmp"):
            continue
        if segment == "absolute":
            absolute.add(symbol)
            continue
        if segment == "__ZITER":
            iterable.setdefault(section, []).append(symbol)
            continue
        if segment not in REGISTRATION:
            continue
        pattern, output_pattern = REGISTRATION[segment]
        match = pattern.fullmatch(section)
        if not match:
            continue
        values = tuple(int(value) for value in match.groups())
        output_section = output_pattern.format(values[0])
        renames.add((segment, section, segment, output_section))
        order.append((segment, values, symbol))

    for section, names in iterable.items():
        for symbol in sorted(set(names)):
            order.append(("__ZITER", (section,), symbol))

    options = []
    if absolute:
        unexported_path = order_path + ".unexported"
        with open(unexported_path, "w", encoding="utf-8") as output:
            for symbol in sorted(absolute):
                output.write(symbol + "\n")
        options.append(f"-Wl,-unexported_symbols_list,{unexported_path}")

    # ld ignores atom ordering during -r. Keep priority-bearing section names
    # intact so the final link can still sort them. Equal priorities retain
    # input order, as SORT_BY_NAME does for identical ELF input section names.
    if not order or relocatable:
        return options

    order.sort(key=lambda entry: (entry[0], entry[1]))
    with open(order_path, "w", encoding="utf-8") as output:
        for _, _, symbol in order:
            output.write(symbol + "\n")

    options.append(f"-Wl,-order_file,{order_path}")
    for old_segment, old_section, new_segment, new_section in sorted(renames):
        options.append(
            f"-Wl,-rename_section,{old_segment},{old_section},"
            f"{new_segment},{new_section}"
        )
    return options


def main():
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--nm", required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    output = next((command[i + 1] for i, arg in enumerate(command[:-1]) if arg == "-o"), None)
    if output:
        order_path = os.path.abspath(output) + ".macho-order.txt"
    else:
        order_path = os.path.join(tempfile.gettempdir(), "macho-order.txt")
    symbols = read_symbols(args.nm, input_files(command))
    relocatable = any(arg == '-r' or (arg.startswith('-Wl,') and '-r' in arg.split(','))
                      for arg in command)
    options = registration_options(symbols, order_path, relocatable)
    return subprocess.call([args.cc, *command, *options])


if __name__ == "__main__":
    sys.exit(main())
