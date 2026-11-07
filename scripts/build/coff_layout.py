#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Lay out COFF link inputs for LLD, which takes neither linker scripts nor -r.

GNU ld builds the Windows native_sim image with a relocatable link driven by
Zephyr's linker script, followed by objcopy symbol localization and a final link
with a second script. LLD's COFF linker supports none of these. It does, however,
merge every input section named "<name>$<suffix>" into the output section <name>,
ordered by the full section name, and it never discards a non-COMDAT section.

The layout command applies a linker script by renaming each input section that the
script places to "<group>$~<position>[<sort key>]", where <group> is the standard
output section for its contents and <position> encodes where the script puts it.
Symbols the script defines, data it emits and alignment it requests become labels
and contents of empty or small marker sections at their own positions. The inputs
are written to a staging directory together with the marker object, and an image
is linked from them for the analysis steps that read the kernel image.

The finalize command makes the staged embedded software private to its image, as
objcopy --localize-symbol does for GNU ld: every symbol it defines outside the
native simulator interface sections is renamed, together with its references. It
then lays out the runner with the native simulator's own script and writes the
response file for the final link.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import shlex
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "pylib" / "build_helpers"))
import coff_edit as ce  # noqa: E402
import ld_script as ld  # noqa: E402

GROUP_PREFIX = {"text": ".text$~", "rdata": ".rdata$~", "data": ".data$~", "bss": ".bss$~"}
GROUP_FLAGS = {
    "text": ce.SCN_CNT_CODE | ce.SCN_MEM_EXECUTE | ce.SCN_MEM_READ,
    "rdata": ce.SCN_CNT_INITIALIZED_DATA | ce.SCN_MEM_READ,
    "data": ce.SCN_CNT_INITIALIZED_DATA | ce.SCN_MEM_READ | ce.SCN_MEM_WRITE,
    "bss": ce.SCN_CNT_UNINITIALIZED_DATA | ce.SCN_MEM_READ | ce.SCN_MEM_WRITE,
}
# Orphans sort after every script position, which only uses digits after "$~".
ORPHAN_INFIX = "~"
STANDARD_SECTIONS = {
    ".text",
    ".data",
    ".rdata",
    ".bss",
    ".tls",
    ".ctors",
    ".dtors",
    ".eh_frame",
    ".xdata",
    ".pdata",
    ".drectve",
    ".edata",
    ".rsrc",
    ".reloc",
    ".gcc_except_table",
    ".rdata_runtime_pseudo_reloc",
    ".llvm_addrsig",
    ".llvm.call-graph-profile",
}
STANDARD_PREFIXES = (
    ".text$",
    ".data$",
    ".rdata$",
    ".bss$",
    ".tls$",
    ".CRT$",
    ".idata$",
    ".debug",
    ".zdebug",
    ".gnu.",
    ".ctors.",
    ".dtors.",
    ".xdata$",
    ".pdata$",
    ".eh_frame$",
)
PRIVATE_PREFIX = "__nsi_esw."


class LayoutError(ValueError):
    pass


def section_class(flags: int) -> str:
    if flags & (ce.SCN_CNT_CODE | ce.SCN_MEM_EXECUTE):
        return "text"
    if flags & ce.SCN_CNT_UNINITIALIZED_DATA and not flags & ce.SCN_CNT_INITIALIZED_DATA:
        return "bss"
    if flags & ce.SCN_MEM_WRITE:
        return "data"
    return "rdata"


def ignorable(section: ce.Section) -> bool:
    """Sections no script places: linker directives and debug information."""
    return bool(section.flags & (ce.SCN_LNK_INFO | ce.SCN_LNK_REMOVE | ce.SCN_MEM_DISCARDABLE))


# --- inputs -------------------------------------------------------------------------


class Unit:
    """One object file, given directly or as an archive member."""

    def __init__(self, input_index: int, member: str | None, obj: ce.CoffObject):
        self.input_index = input_index
        self.member = member
        self.obj = obj


class Inputs:
    def __init__(self):
        self.files: list[dict] = []  # {"path": original path, "whole": bool, "archive": bool}
        self.units: list[Unit] = []

    def add(self, path: pathlib.Path, whole: bool) -> None:
        data = path.read_bytes()
        index = len(self.files)
        archive = data.startswith(b"!<arch>\n")
        self.files.append({"path": str(path), "whole": whole, "archive": archive})
        try:
            if archive:
                for member, content in ce.read_archive(data):
                    self.units.append(Unit(index, member, ce.CoffObject(content)))
            else:
                self.units.append(Unit(index, None, ce.CoffObject(data)))
        except ValueError as error:
            raise LayoutError(f"{path}: {error}") from error

    def write(self, stage: pathlib.Path, prefix: str = "") -> list[dict]:
        """Write every input to stage, returning their descriptions in input order."""
        stage.mkdir(parents=True, exist_ok=True)
        written = []
        for index, entry in enumerate(self.files):
            units = [unit for unit in self.units if unit.input_index == index]
            name = f"{prefix}{index:03d}-{pathlib.Path(entry['path']).name}"
            target = stage / name
            if entry["archive"]:
                data = ce.write_archive([(unit.member, unit.obj.to_bytes()) for unit in units])
            else:
                data = units[0].obj.to_bytes()
            replace_if_changed(target, data)
            written.append({**entry, "staged": str(target)})
        return written


def replace_if_changed(path: pathlib.Path, data: bytes) -> None:
    """Keep the timestamp of unchanged files so the final link is not redone needlessly."""
    if path.is_file() and path.read_bytes() == data:
        return
    temp = path.with_name(path.name + ".tmp")
    temp.write_bytes(data)
    os.replace(temp, path)


# --- linker script application ------------------------------------------------------


class Layout:
    """Apply linker scripts to COFF inputs by renaming sections and adding markers."""

    def __init__(self, scripts: list[str], namespace: str):
        # Layouts linked into one image need disjoint positions: the namespace is
        # the first character of every position this layout assigns.
        if len(namespace) != 1 or not namespace.isalnum():
            raise LayoutError(f"invalid layout namespace {namespace!r}")
        self.namespace = namespace
        self.commands: list[ld.Sections] = []
        for text in scripts:
            self.commands.extend(ld.parse(text))
        self.renamed: dict[str, str] = {}  # new section name -> original name
        self.marker_sections: list[ce.Section] = []
        self.marker_symbols: list[ce.Symbol] = []

    def apply(self, units: list[Unit], orphans: bool = True) -> None:
        self.labels: dict[str, tuple[str, int]] = {}  # symbol -> (output section, offset)
        self.sizes: dict[str, int] = {}
        # Every input section description, in script order, with a sortable position.
        descriptions = []
        commands = []
        for c, command in enumerate(self.commands):
            items = attach_assignments(command.items)
            outputs = []
            for s, item in enumerate(items):
                if isinstance(item, ld.Assert):
                    outputs.append((None, item))
                    continue
                key = f"{self.namespace}{c:02d}{s:04d}"
                outputs.append((key, item))
                for i, statement in enumerate(item.items):
                    if isinstance(statement, ld.Input):
                        if statement.file_pattern != "*":
                            raise LayoutError(
                                f"unsupported file pattern {statement.file_pattern!r}"
                            )
                        for k, spec in enumerate(statement.specs):
                            if spec.exclude_files:
                                raise LayoutError("EXCLUDE_FILE is not supported")
                            descriptions.append((f"{key}{i:04d}", k, spec))
            commands.append(outputs)
        placed: dict[str, list] = {}
        for unit in units:
            for index, section in enumerate(unit.obj.sections):
                if ignorable(section):
                    continue
                for item_key, k, spec in descriptions:
                    if ld.matches(spec, section.name):
                        placed.setdefault(item_key[:7], []).append((item_key, k, spec, unit, index))
                        break
                else:
                    if orphans:
                        self._orphan(unit, index)
        for outputs in commands:
            for key, item in outputs:
                if key is None:
                    self.check(item)
                else:
                    self._place(key, item, placed.get(key, []))

    def _orphan(self, unit: Unit, index: int) -> None:
        section = unit.obj.sections[index]
        name = section.name
        if name in STANDARD_SECTIONS or name.startswith(STANDARD_PREFIXES):
            return
        # GNU ld makes an output section of each orphan name. A PE image holds few
        # sections, so orphans join the standard section for their contents instead.
        new = GROUP_PREFIX[section_class(section.flags)] + ORPHAN_INFIX + name
        unit.obj.rename_section(index, new)
        self.renamed[new] = name

    def _place(self, key: str, output: ld.OutputSection, matched: list) -> None:
        if output.discard:
            for *_, unit, index in matched:
                unit.obj.sections[index].flags |= ce.SCN_LNK_REMOVE
            return
        classes = {section_class(unit.obj.sections[index].flags) for *_, unit, index in matched}
        has_data = any(isinstance(item, ld.Data) for item in output.items)
        if "text" in classes and (len(classes) > 1 or has_data):
            raise LayoutError(f"output section {output.name} mixes code and data")
        if classes == {"text"}:
            group = "text"
        elif classes == {"bss"} and not has_data:
            group = "bss"
        elif classes == {"rdata"} and not has_data:
            group = "rdata"
        else:
            group = "data"
        if output.noload and group != "bss":
            raise LayoutError(f"NOLOAD output section {output.name} has initialized contents")
        subalign = ld.constant(output.subalign) if output.subalign else None
        start_alignment = ld.constant(output.align) if output.align else 1

        # Rename the matched input sections into the group, in script order.
        sized = {}  # item key -> list of (sort key, size, alignment)
        for item_key, k, spec, unit, index in matched:
            section = unit.obj.sections[index]
            if group != "bss" and section.data is None:
                unit.obj.make_initialized(index)
            align = subalign or ce.alignment(section.flags)
            flags = GROUP_FLAGS[group] | (section.flags & ce.SCN_LNK_COMDAT)
            section.flags = ce.with_alignment(flags, align)
            start_alignment = max(start_alignment, align)
            sort = sort_key(spec, section.name, align)
            new = f"{GROUP_PREFIX[group]}{item_key}{k:02d}{sort}"
            self.renamed[new] = section.name
            unit.obj.rename_section(index, new)
            sized.setdefault(item_key, []).append((f"{k:02d}{sort}", section.size, align))

        # Emit one marker section for each run of statements between input descriptions,
        # and compute every position in the output section to evaluate assertions.
        offset = 0
        labels = self.labels
        run: list = []
        run_key = None
        first = True

        def flush():
            nonlocal run, run_key, offset, first
            if not run:
                return
            data = bytearray()
            align = start_alignment if first else 1
            if first:
                offset = 0
            local: dict[str, int] = {}
            for item in run:
                if isinstance(item, ld.Assign):
                    if item.expr.kind == "dot":
                        local[item.name] = len(data)
                    elif item.expr.kind == "symbol" and item.expr.value in local:
                        local[item.name] = local[item.expr.value]
                    elif item.expr.kind == "symbol" and item.expr.value in labels:
                        self.alias(item.name, item.expr.value)
                        labels[item.name] = labels[item.expr.value]
                    else:
                        raise LayoutError(f"unsupported assignment to {item.name} in {output.name}")
                elif isinstance(item, ld.Dot):
                    if item.expr.kind != "align":
                        raise LayoutError(f"unsupported location counter change in {output.name}")
                    value = ld.constant(item.expr.value)
                    if not data and not local:
                        align = max(align, value)
                    else:
                        data.extend(bytes(-(offset + len(data)) % value))
                elif isinstance(item, ld.Data):
                    data.extend(ld.constant(item.expr).to_bytes(item.size, "little"))
            offset += -offset % align
            name = f"{GROUP_PREFIX[group]}{run_key}"
            if group == "bss" and any(data):
                raise LayoutError(f"data statement in uninitialized section {output.name}")
            payload = None if group == "bss" else bytes(data)
            self.marker_sections.append(
                ce.Section(
                    name,
                    ce.with_alignment(GROUP_FLAGS[group], align),
                    len(data),
                    payload,
                    b"",
                    b"",
                    0,
                )
            )
            number = len(self.marker_sections)
            for symbol, position in local.items():
                self.marker_symbols.append(
                    ce.Symbol(symbol, position, number, 0, ce.SYM_CLASS_EXTERNAL, b"")
                )
                labels[symbol] = (output.name, offset + position)
            offset += len(data)
            run = []
            run_key = None
            first = False

        for i, item in enumerate(output.items):
            item_key = f"{key}{i:04d}"
            if isinstance(item, ld.Input):
                flush()
                first = False
                for _, size, align in sorted(sized.get(item_key, [])):
                    offset += -offset % align + size
                continue
            if isinstance(item, ld.Assert):
                flush()
                self.sizes[output.name] = offset
                self.check(item)
                continue
            if run_key is None:
                run_key = item_key
            run.append(item)
        flush()
        self.sizes[output.name] = offset

    def alias(self, name: str, target: str) -> None:
        for symbol in self.marker_symbols:
            if symbol.name == target:
                self.marker_symbols.append(
                    ce.Symbol(name, symbol.value, symbol.section, 0, ce.SYM_CLASS_EXTERNAL, b"")
                )
                return
        raise LayoutError(f"cannot alias {name} to {target}")

    def check(self, item: ld.Assert) -> None:
        """Evaluate an ASSERT. Symbols compare by position within their output section."""

        def value(expr: ld.Expr):
            if expr.kind == "number":
                return expr.value
            if expr.kind == "symbol":
                if expr.value not in self.labels:
                    raise LayoutError(f"ASSERT uses unknown symbol {expr.value}")
                return self.labels[expr.value]
            if expr.kind == "sizeof":
                if expr.value not in self.sizes:
                    raise LayoutError(f"ASSERT uses SIZEOF of unknown section {expr.value}")
                return self.sizes[expr.value]
            if expr.kind == "binary":
                left, right = value(expr.left), value(expr.right)
                if expr.value in ("==", "!="):
                    if type(left) is not type(right) or (
                        isinstance(left, tuple) and left[0] != right[0]
                    ):
                        raise LayoutError("ASSERT compares positions in different sections")
                    return int((left == right) == (expr.value == "=="))
                if isinstance(left, int) and isinstance(right, int):
                    return left + right if expr.value == "+" else left - right
            raise LayoutError(f"unsupported ASSERT expression {expr.kind}")

        if not value(item.expr):
            raise LayoutError(f"linker script assertion failed: {item.message}")

    def markers(self) -> bytes:
        return ce.write_object(self.marker_sections, self.marker_symbols)


def attach_assignments(items: list) -> list:
    """Move assignments between output sections into the neighbouring output section.

    A symbol assigned between two output sections takes the address that follows the
    previous one, which is where a trailing assignment inside it puts the symbol.
    """
    result = []
    pending = []
    for item in items:
        if isinstance(item, (ld.Assign, ld.Dot)):
            if result and isinstance(result[-1], ld.OutputSection) and not result[-1].discard:
                previous = result[-1]
                result[-1] = ld.OutputSection(
                    previous.name,
                    previous.items + [item],
                    previous.align,
                    previous.subalign,
                    previous.noload,
                )
            else:
                pending.append(item)
        elif isinstance(item, ld.OutputSection):
            if pending and not item.discard:
                item = ld.OutputSection(
                    item.name, pending + item.items, item.align, item.subalign, item.noload
                )
                pending = []
            result.append(item)
        else:
            result.append(item)
    if pending:
        raise LayoutError("assignments after the last output section are not supported")
    return result


def sort_key(spec: ld.SectionSpec, name: str, align: int) -> str:
    key = ""
    for sort in spec.sort:
        if sort in ("SORT", "SORT_BY_NAME"):
            key += "." + name
        elif sort == "SORT_BY_ALIGNMENT":
            key += f"{99 - align.bit_length():02d}"
        elif sort == "SORT_BY_INIT_PRIORITY":
            key += f"{ld.init_priority(name):05d}"
    return key


# --- command line -------------------------------------------------------------------


def split_windows(text: str) -> list[str]:
    """Split a response file as the Windows C runtime splits a command line."""
    tokens = []
    current = []
    in_token = in_quotes = False
    index = 0
    while index < len(text):
        character = text[index]
        if character == "\\":
            count = 0
            while index < len(text) and text[index] == "\\":
                count += 1
                index += 1
            if index < len(text) and text[index] == '"':
                current.append("\\" * (count // 2))
                if count % 2:
                    current.append('"')
                    index += 1
            else:
                current.append("\\" * count)
            in_token = True
            continue
        if character == '"':
            in_quotes = not in_quotes
            in_token = True
        elif character.isspace() and not in_quotes:
            if in_token:
                tokens.append("".join(current))
                current = []
                in_token = False
        else:
            current.append(character)
            in_token = True
        index += 1
    if in_token:
        tokens.append("".join(current))
    return tokens


def expand_response_files(arguments: list[str]) -> list[str]:
    result = []
    for argument in arguments:
        if argument.startswith("@") and pathlib.Path(argument[1:]).is_file():
            text = pathlib.Path(argument[1:]).read_text(encoding="utf-8")
            words = split_windows(text) if os.name == "nt" else shlex.split(text)
            result.extend(expand_response_files(words))
        else:
            result.append(argument)
    return result


def parse_link_command(arguments: list[str]) -> dict:
    """Extract scripts, inputs and undefined symbols from a GNU style link command."""
    parsed = {
        "scripts": [],
        "inputs": [],
        "undefined": [],
        "map": None,
        "output": None,
        "driver": [],
        "ignored": [],
    }
    whole = False
    items = iter(expand_response_files(arguments))
    for argument in items:
        if argument == "-T":
            parsed["scripts"].append(next(items))
        elif argument.startswith("-T") and len(argument) > 2:
            parsed["scripts"].append(argument[2:])
        elif argument == "-o":
            parsed["output"] = next(items)
        elif argument.startswith("-Wl,"):
            options = argument[4:].split(",")
            index = 0
            while index < len(options):
                option = options[index]
                if option == "--whole-archive":
                    whole = True
                elif option == "--no-whole-archive":
                    whole = False
                elif option in ("-u", "--undefined"):
                    index += 1
                    parsed["undefined"].append(options[index])
                elif option.startswith("--undefined="):
                    parsed["undefined"].append(option.split("=", 1)[1])
                elif option.startswith(("-Map=", "--Map=")):
                    parsed["map"] = option.split("=", 1)[1]
                elif option in ("-T", "--script"):
                    index += 1
                    # -Wl,-T may leave the file name to the next argument.
                    parsed["scripts"].append(
                        options[index] if index < len(options) else next(items)
                    )
                else:
                    parsed["ignored"].append(f"-Wl,{option}")
                index += 1
        elif argument.startswith(("-l", "-L")) or argument in ("-r", "-nostdlib", "-shared"):
            # Host libraries belong to the final image link, not to the embedded software.
            parsed["ignored"].append(argument)
        elif argument in ("-target", "--target", "-Xlinker"):
            parsed["driver"] += [argument, next(items)]
        elif argument.startswith("-"):
            # Compiler driver options such as --target, --config and -m32 also select
            # the target and runtime of the kernel image link.
            parsed["driver"].append(argument)
        elif not argument:
            continue
        else:
            parsed["inputs"].append((argument, whole))
    return parsed


def rsp_quote(path: str) -> str:
    path = str(path).replace("\\", "/")
    if any(character in path for character in ' "\t\n'):
        raise LayoutError(f"unsupported character in path {path!r}")
    return path


def run_quiet(command: list[str], what: str) -> None:
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        sys.stderr.write(result.stdout)
        raise LayoutError(f"{what} failed with exit status {result.returncode}")


def split_compiler(command: list[str]) -> tuple[list[str], list[str]]:
    """Split the compiler driver (with any launcher such as ccache) from its arguments."""
    for index, argument in enumerate(command):
        if argument.startswith(("-", "@")) or argument.lower().endswith(
            (".o", ".obj", ".a", ".lib")
        ):
            if index == 0:
                raise LayoutError("the link command does not start with the compiler")
            return command[:index], command[index:]
    raise LayoutError("the link command has no arguments")


def layout_command(args) -> None:
    compiler, arguments = split_compiler(args.link)
    link = parse_link_command(arguments)
    # The manifest is read from another build directory: record absolute paths.
    output = pathlib.Path(os.path.abspath(args.output or link["output"]))
    if not link["scripts"]:
        raise LayoutError("the link command names no linker script")
    stage = pathlib.Path(os.path.abspath(args.stage or f"{output}.coff-layout"))
    inputs = Inputs()
    for path, whole in link["inputs"]:
        inputs.add(pathlib.Path(os.path.abspath(path)), whole)
    layout = Layout(
        [pathlib.Path(script).read_text(encoding="utf-8") for script in link["scripts"]],
        args.namespace,
    )
    layout.apply(inputs.units)
    staged = inputs.write(stage)
    markers = stage / "layout-markers.obj"
    replace_if_changed(markers, layout.markers())
    manifest = {
        "inputs": staged,
        "markers": str(markers),
        "undefined": link["undefined"],
        "sections": layout.renamed,
        "scripts": link["scripts"],
    }
    manifest_path = pathlib.Path(f"{output}.coff-layout.json")
    replace_if_changed(manifest_path, (json.dumps(manifest, indent=1) + "\n").encode())

    # The kernel image: the embedded software alone, with references to the runner
    # left unresolved. It is only read by analysis steps; the runner links its own.
    rsp = stage / "image.rsp"
    lines = image_link_arguments(manifest, private={})
    if link["map"]:
        lines.append(f"-Wl,-Map={rsp_quote(link['map'])}")
    rsp.write_text("\n".join(lines) + "\n", encoding="utf-8")
    run_quiet(
        [
            *compiler,
            *link["driver"],
            "-shared",
            "-o",
            str(output),
            f"@{rsp}",
            "-Wl,--no-gc-sections",
            "-Wl,--no-insert-timestamp",
            "-Xlinker",
            "-Xlink=-force:unresolved",
            *args.image_option,
        ],
        "linking the kernel image",
    )


def image_link_arguments(manifest: dict, private: dict) -> list[str]:
    whole = [entry for entry in manifest["inputs"] if entry["whole"] or not entry["archive"]]
    rest = [entry for entry in manifest["inputs"] if not (entry["whole"] or not entry["archive"])]
    lines = ["-Wl,--whole-archive"]
    lines += [rsp_quote(entry["staged"]) for entry in whole]
    lines.append(rsp_quote(manifest["markers"]))
    lines.append("-Wl,--no-whole-archive")
    lines += [rsp_quote(entry["staged"]) for entry in rest]
    lines += [f"-Wl,-u,{private.get(name, name)}" for name in manifest["undefined"]]
    return lines


# --- localization -------------------------------------------------------------------


def localize(units: list[Unit], interface_sections, prefix: str) -> dict[str, str]:
    """Rename the symbols the units define outside interface sections, with their uses."""
    defined = set()
    public = set()
    for unit in units:
        obj = unit.obj
        for symbol in obj.defined_symbols():
            defined.add(symbol.name)
            section = obj.section_of(symbol)
            if section is not None and interface_sections(section.name):
                public.add(symbol.name)
        # A weak external is public when its fallback definition is an interface.
        for symbol in obj.symbols:
            if symbol is not None and symbol.storage == ce.SYM_CLASS_WEAK_EXTERNAL:
                target = int.from_bytes(symbol.aux[0:4], "little")
                fallback = obj.symbols[target]
                section = obj.section_of(fallback) if fallback is not None else None
                if section is not None and interface_sections(section.name):
                    public.update((symbol.name, fallback.name))
    names = {name: prefix + name for name in defined - public}
    names.update({"__imp_" + name: "__imp_" + prefix + name for name in defined - public})
    for unit in units:
        for symbol in unit.obj.symbols:
            if (
                symbol is not None
                and symbol.name in names
                and symbol.storage
                in (
                    ce.SYM_CLASS_EXTERNAL,
                    ce.SYM_CLASS_WEAK_EXTERNAL,
                )
            ):
                symbol.name = names[symbol.name]
        for section in unit.obj.sections:
            if section.name == ".drectve" and section.data:
                for directive in section.data.decode("latin-1").split():
                    match = re.match(r"[-/](?:export|include):\"?([^,\"]+)", directive, re.I)
                    if match and match.group(1) in names:
                        raise LayoutError(f"linker directive {directive} names a private symbol")
    return names


def finalize_command(args) -> None:
    manifest = json.loads(pathlib.Path(args.manifest).read_text(encoding="utf-8"))
    stage = pathlib.Path(os.path.abspath(args.stage))
    original = manifest["sections"]

    embedded = Inputs()
    for entry in manifest["inputs"]:
        embedded.add(pathlib.Path(entry["staged"]), entry["whole"])
    markers = Inputs()
    markers.add(pathlib.Path(manifest["markers"]), True)
    names = localize(
        embedded.units + markers.units,
        lambda name: original.get(name, name).startswith(args.interface_prefix),
        args.prefix,
    )
    staged = embedded.write(stage, "esw-")
    for entry, previous in zip(staged, manifest["inputs"], strict=True):
        entry["whole"] = previous["whole"]
        entry["archive"] = previous["archive"]
    marker_file = stage / "esw-markers.obj"
    replace_if_changed(marker_file, markers.units[0].obj.to_bytes())

    runner = Inputs()
    for path in args.runner:
        runner.add(pathlib.Path(path), True)
    layout = Layout(
        [pathlib.Path(script).read_text(encoding="utf-8") for script in args.script], args.namespace
    )
    layout.apply(runner.units)
    runner_staged = runner.write(stage, "runner-")
    runner_markers = stage / "runner-markers.obj"
    replace_if_changed(runner_markers, layout.markers())

    localized = {**manifest, "inputs": staged, "markers": str(marker_file)}
    lines = image_link_arguments(localized, names)
    lines[lines.index("-Wl,--no-whole-archive") : lines.index("-Wl,--no-whole-archive")] = [
        rsp_quote(entry["staged"]) for entry in runner_staged
    ] + [rsp_quote(runner_markers)]
    # Always rewritten: its time stamp tells the final links that an input changed.
    pathlib.Path(args.rsp).write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0], allow_abbrev=False)
    commands = parser.add_subparsers(dest="command", required=True)
    layout = commands.add_parser("layout", help="replace a relocatable link with a linker script")
    layout.add_argument("--output", help="kernel image (default: the link command's -o)")
    layout.add_argument("--stage", help="directory for the laid out inputs")
    layout.add_argument(
        "--image-option", action="append", default=[], help="extra image link option"
    )
    layout.add_argument("--namespace", default="e", help="first character of the section positions")
    layout.add_argument(
        "link", nargs=argparse.REMAINDER, help="-- followed by the link command, compiler first"
    )
    finalize = commands.add_parser(
        "finalize", help="localize the embedded software and add the runner"
    )
    finalize.add_argument("--manifest", required=True)
    finalize.add_argument("--stage", required=True)
    finalize.add_argument("--script", action="append", default=[], help="runner linker script")
    finalize.add_argument("--runner", action="append", default=[], help="runner archive or object")
    finalize.add_argument("--rsp", required=True, help="response file for the final link")
    finalize.add_argument("--interface-prefix", default=".native_sim_if")
    finalize.add_argument("--prefix", default=PRIVATE_PREFIX)
    finalize.add_argument(
        "--namespace", default="r", help="first character of the runner positions"
    )
    args = parser.parse_args()
    try:
        if args.command == "layout":
            if args.link and args.link[0] == "--":
                args.link = args.link[1:]
            layout_command(args)
        else:
            finalize_command(args)
    except (LayoutError, ld.ScriptError, ValueError, OSError) as error:
        sys.exit(f"coff_layout.py: error: {error}")


if __name__ == "__main__":
    main()
