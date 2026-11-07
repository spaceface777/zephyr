# SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

"""Parse the subset of GNU ld scripts that Zephyr generates for the POSIX architecture.

The parser covers SECTIONS commands with output section descriptions, input section
descriptions with KEEP and the SORT variants, symbol assignments, PROVIDE, data
statements, location counter alignment, ASSERT and INSERT. Anything else is rejected,
so a script feature this module does not model cannot be silently ignored.
"""

from __future__ import annotations

import fnmatch
import re
from dataclasses import dataclass, field

_TOKEN = re.compile(
    r"""
    (?P<space>\s+|/\*.*?\*/)
  | (?P<string>"[^"\n]*")
  | (?P<op>==|!=|<=|>=|&&|\|\||[{}();=,:<>+])
  | (?P<word>[A-Za-z0-9_.$*?\[\]\-/~^!]+)
    """,
    re.VERBOSE | re.DOTALL,
)

SORT_KEYWORDS = {
    "SORT",
    "SORT_BY_NAME",
    "SORT_BY_ALIGNMENT",
    "SORT_BY_INIT_PRIORITY",
    "SORT_NONE",
}
DATA_SIZES = {"BYTE": 1, "SHORT": 2, "LONG": 4, "QUAD": 8, "SQUAD": 8}


class ScriptError(ValueError):
    pass


@dataclass
class Expr:
    """A location counter expression: kind is dot, symbol, number, align, sizeof or binary."""

    kind: str
    value: object = None
    left: Expr | None = None
    right: Expr | None = None


@dataclass
class SectionSpec:
    """One section pattern list of an input section description, with its sort order."""

    patterns: list[str]
    sort: tuple[str, ...] = ()
    exclude_files: list[str] = field(default_factory=list)


@dataclass
class Input:
    file_pattern: str
    specs: list[SectionSpec]
    keep: bool


@dataclass
class Assign:
    name: str
    expr: Expr
    provide: bool = False


@dataclass
class Dot:
    expr: Expr


@dataclass
class Data:
    size: int
    expr: Expr


@dataclass
class Assert:
    expr: Expr
    message: str


@dataclass
class OutputSection:
    name: str
    items: list
    align: Expr | None = None
    subalign: Expr | None = None
    noload: bool = False

    @property
    def discard(self) -> bool:
        return self.name == "/DISCARD/"


@dataclass
class Sections:
    """One SECTIONS command; items are OutputSection, Assign, Dot or Assert."""

    items: list
    insert: tuple[str, str] | None = None


def tokenize(text: str) -> list[tuple[str, str]]:
    tokens = []
    position = 0
    while position < len(text):
        match = _TOKEN.match(text, position)
        if not match:
            raise ScriptError(f"unexpected character {text[position]!r} at offset {position}")
        position = match.end()
        kind = match.lastgroup
        if kind != "space":
            tokens.append((kind, match.group(kind)))
    return tokens


class _Parser:
    def __init__(self, text: str):
        self.tokens = tokenize(text)
        self.position = 0

    def peek(self, offset: int = 0) -> str | None:
        index = self.position + offset
        return self.tokens[index][1] if index < len(self.tokens) else None

    def next(self) -> str:
        if self.position >= len(self.tokens):
            raise ScriptError("unexpected end of script")
        token = self.tokens[self.position][1]
        self.position += 1
        return token

    def expect(self, value: str) -> None:
        token = self.next()
        if token != value:
            raise ScriptError(f"expected {value!r}, found {token!r}")

    def accept(self, value: str) -> bool:
        if self.peek() == value:
            self.position += 1
            return True
        return False

    def script(self) -> list[Sections]:
        commands = []
        while self.peek() is not None:
            if self.accept(";"):
                continue
            keyword = self.next()
            if keyword != "SECTIONS":
                raise ScriptError(f"unsupported linker script command {keyword!r}")
            self.expect("{")
            items = []
            while not self.accept("}"):
                item = self.section_item()
                if item is not None:
                    items.append(item)
            insert = None
            if self.accept("INSERT"):
                where = self.next()
                if where not in ("AFTER", "BEFORE"):
                    raise ScriptError(f"unsupported INSERT {where}")
                insert = (where, self.next())
            self.accept(";")
            commands.append(Sections(items, insert))
        return commands

    # Statements valid both at SECTIONS level and inside output sections.
    def common_statement(self):
        token = self.peek()
        if token == ";":
            self.next()
            return None
        if token in ("PROVIDE", "PROVIDE_HIDDEN", "HIDDEN"):
            self.next()
            self.expect("(")
            name = self.next()
            self.expect("=")
            expr = self.expr()
            self.expect(")")
            self.accept(";")
            return Assign(name, expr, provide=token != "HIDDEN")
        if token == "ASSERT":
            self.next()
            self.expect("(")
            expr = self.expr()
            self.expect(",")
            message = self.next()
            self.expect(")")
            self.accept(";")
            return Assert(expr, message.strip('"'))
        if token == "." and self.peek(1) == "=":
            self.next()
            self.next()
            expr = self.expr()
            self.expect(";")
            return Dot(expr)
        if self.peek(1) == "=" and token is not None and re.fullmatch(r"[A-Za-z_.$][\w.$]*", token):
            name = self.next()
            self.next()
            expr = self.expr()
            self.expect(";")
            return Assign(name, expr)
        return False

    def section_item(self):
        statement = self.common_statement()
        if statement is not False:
            return statement
        name = self.next()
        noload = False
        if self.accept("("):
            kind = self.next()
            if kind not in ("NOLOAD", "COPY", "INFO", "OVERLAY", "READONLY"):
                raise ScriptError(f"unsupported output section type {kind}")
            noload = kind == "NOLOAD"
            self.expect(")")
        if self.peek() != ":":
            # An output section address (such as PE's BLOCK(__section_alignment__))
            # places the whole section; grouped COFF sections have no address of their own.
            self.expr()
        self.expect(":")
        align = subalign = None
        while self.peek() != "{":
            keyword = self.next()
            if keyword in ("ALIGN", "SUBALIGN"):
                self.expect("(")
                value = self.expr()
                self.expect(")")
                if keyword == "ALIGN":
                    align = value
                else:
                    subalign = value
            elif keyword == "AT":
                self.expect("(")
                self.expr()
                self.expect(")")
            elif keyword not in ("ONLY_IF_RO", "ONLY_IF_RW"):
                raise ScriptError(f"unsupported attribute {keyword!r} of output section {name}")
        self.expect("{")
        items = []
        while not self.accept("}"):
            item = self.output_item()
            if item is not None:
                items.append(item)
        # Memory regions, load regions, program headers and fill have no COFF meaning.
        while True:
            if self.accept(">"):
                self.next()
            elif self.peek() == "AT" and self.peek(1) == ">":
                self.position += 2
                self.next()
            elif self.peek() == ":" and self.peek(1) not in (None, "{"):
                self.next()
                self.next()
            elif self.accept("="):
                self.expr()
            else:
                break
        self.accept(",")
        self.accept(";")
        return OutputSection(name, items, align, subalign, noload)

    def output_item(self):
        statement = self.common_statement()
        if statement is not False:
            return statement
        token = self.peek()
        if token in DATA_SIZES:
            self.next()
            self.expect("(")
            expr = self.expr()
            self.expect(")")
            self.accept(";")
            return Data(DATA_SIZES[token], expr)
        if token == "KEEP":
            self.next()
            self.expect("(")
            item = self.input_description(keep=True)
            self.expect(")")
            self.accept(";")
            return item
        item = self.input_description(keep=False)
        self.accept(";")
        return item

    def input_description(self, keep: bool) -> Input:
        file_pattern = self.next()
        if file_pattern in SORT_KEYWORDS or file_pattern == "EXCLUDE_FILE":
            raise ScriptError(f"unsupported file pattern {file_pattern}")
        if file_pattern.startswith('"'):
            file_pattern = file_pattern.strip('"')
        self.expect("(")
        specs = []
        loose = []
        while not self.accept(")"):
            token = self.peek()
            if token in SORT_KEYWORDS:
                specs.append(self.sorted_spec())
            elif token == "EXCLUDE_FILE":
                self.next()
                self.expect("(")
                excluded = []
                while not self.accept(")"):
                    excluded.append(self.next())
                pattern = self.next().strip('"')
                specs.append(SectionSpec([pattern], (), excluded))
            else:
                loose.append(self.next().strip('"'))
        if loose:
            # Unsorted patterns of one description keep their sections in input order.
            specs.insert(0, SectionSpec(loose))
        return Input(file_pattern, specs, keep)

    def sorted_spec(self) -> SectionSpec:
        sort = []
        depth = 0
        while self.peek() in SORT_KEYWORDS:
            sort.append(self.next())
            self.expect("(")
            depth += 1
        patterns = []
        while self.peek() != ")":
            patterns.append(self.next().strip('"'))
        for _ in range(depth):
            self.expect(")")
        if not patterns:
            raise ScriptError("empty SORT pattern list")
        return SectionSpec(patterns, tuple(s for s in sort if s != "SORT_NONE"))

    def expr(self) -> Expr:
        left = self.primary()
        while self.peek() in ("==", "!=", "+", "-"):
            op = self.next()
            left = Expr("binary", op, left, self.primary())
        return left

    def primary(self) -> Expr:
        token = self.next()
        if token == "(":
            value = self.expr()
            self.expect(")")
            return value
        if token == ".":
            return Expr("dot")
        if token in ("ALIGN", "BLOCK", "SIZEOF", "ABSOLUTE", "ADDR"):
            self.expect("(")
            if token in ("SIZEOF", "ADDR"):
                argument = self.next()
                self.expect(")")
                return Expr(token.lower(), argument)
            value = self.expr()
            self.expect(")")
            if token == "ABSOLUTE":
                return value
            return Expr("align", value)
        if re.fullmatch(r"0[xX][0-9a-fA-F]+|[0-9]+[KM]?", token):
            scale = {"K": 1024, "M": 1024 * 1024}.get(token[-1], 1)
            number = token.rstrip("KM")
            return Expr("number", int(number, 0) * scale)
        if re.fullmatch(r"[A-Za-z_.$][\w.$]*", token):
            return Expr("symbol", token)
        raise ScriptError(f"unsupported expression token {token!r}")


def parse(text: str) -> list[Sections]:
    """Parse a preprocessed linker script into its SECTIONS commands."""
    return _Parser(text).script()


def constant(expr: Expr) -> int:
    if expr.kind == "number":
        return expr.value
    raise ScriptError(f"expected a constant, found {expr.kind}")


def matches(spec: SectionSpec, name: str) -> bool:
    return any(fnmatch.fnmatchcase(name, pattern) for pattern in spec.patterns)


def init_priority(name: str) -> int:
    """GNU ld's SORT_BY_INIT_PRIORITY key for .init_array.N and .ctors.N sections."""
    suffix = name.rsplit(".", 1)[-1]
    if not suffix.isdigit():
        return 65536
    value = int(suffix)
    return 65535 - value if name.startswith((".ctors", ".dtors")) else value
