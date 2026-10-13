#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Generate the Mach-O iterable section bounds an image references.

ld64 has no section bound symbols of its own: a reference to
section$start$<segment>$<section> makes it materialise an output section, and it
runs out of section indexes after 127 of them. Emitting a bound for every
iterable section Zephyr has is well past that, so the bounds are taken from the
symbols the built image leaves undefined, which is exactly the set it uses.
"""

import argparse
import re
import subprocess
import sys
from pathlib import Path

from iter_sections import parse_tagged_items
from order_macho_sections import descendants, object_metadata, section_names

# A bound of the section <name> is _<name>_list_start and so on, and Mach-O
# prefixes a symbol with one more underscore
BOUND_RE = re.compile(r"^__(?P<name>.+)_(?P<bound>list_start|list_end|ext_end)$")

BOUND_KIND = {"list_start": "start", "list_end": "end", "ext_end": "end"}


def read_mapping(path):
    """Return the logical to Mach-O section name mapping."""
    mapping = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        name, short_name = line.split()
        mapping[name] = short_name

    return mapping


def undefined_bounds(nm, image):
    """Return the section bounds the image leaves undefined, as (name, bound)."""
    result = subprocess.run([str(nm), "-u", str(image)], check=True, capture_output=True, text=True)
    bounds = []
    for line in result.stdout.splitlines():
        match = BOUND_RE.fullmatch(line.strip())
        if match is not None:
            bounds.append((match.group("name"), match.group("bound")))

    return sorted(set(bounds))


def bound_target(name, bound, mapping, parent_children, present):
    """Return the section and boundary covering an API class or iterable table."""
    target_name = name
    kind = BOUND_KIND[bound]
    members = [child for child in descendants(name, parent_children) if mapping[child] in present]
    if members:
        if bound == "ext_end":
            target_name = members[-1]
        elif mapping[name] not in present:
            target_name = members[0]
            kind = "start"
    return f"section${kind}$__DATA${mapping[target_name]}"


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--mapping", required=True, type=Path)
    parser.add_argument("--struct-tags", type=Path)
    parser.add_argument("--nm", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def main():
    args = parse_args()
    mapping = read_mapping(args.mapping)
    bounds = undefined_bounds(args.nm, args.image)
    parent_children = {}
    present = set()
    if args.struct_tags is not None:
        _, parent_children, _ = parse_tagged_items(args.struct_tags, "__subsystem")
        if parent_children:
            _, _, sections, _ = object_metadata(args.image.read_bytes())
            present = section_names(sections)

    missing = sorted({name for name, _ in bounds} - mapping.keys())
    if missing:
        sys.exit(
            f"{args.image}: no Mach-O section is mapped for {', '.join(missing)}. "
            "The section is not one gen_macho_iter_sections.py found in the tree."
        )

    lines = [
        "/* Generated file. Do not edit. */",
        "#ifdef __APPLE__",
        "#define MACHO_ALIAS(sym, target) \\",
        '\t__asm__(".globl _" #sym "\\n_" #sym " = " target)',
        "",
    ]
    for name, bound in bounds:
        target = bound_target(name, bound, mapping, parent_children, present)
        lines.append(f'MACHO_ALIAS(_{name}_{bound}, "{target}");')
    lines.extend(["", "#endif /* __APPLE__ */", ""])

    args.output.write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    main()
