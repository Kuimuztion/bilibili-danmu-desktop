#!/usr/bin/env python3
"""Static source-structure checks for function documentation and if nesting."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
SOURCE_FILES = sorted((ROOT / "src").glob("*.c"))
CONTROL_WORDS = {"if", "for", "while", "switch", "sizeof", "return"}

COMMENT_OR_LITERAL = re.compile(
    r"//[^\n]*|/\*[\s\S]*?\*/|\"(?:\\.|[^\"\\])*\"|"
    r"'(?:\\.|[^'\\])*'"
)


def mask_comments_and_literals(source: str) -> str:
    """Keep offsets/newlines while hiding comments and C literals."""
    return COMMENT_OR_LITERAL.sub(
        lambda match: re.sub(r"[^\n]", " ", match.group()), source
    )


def brace_pairs(masked: str) -> dict[int, int]:
    """Return opening-brace to matching-closing-brace source offsets."""
    stack: list[int] = []
    pairs: dict[int, int] = {}
    for offset, char in enumerate(masked):
        if char == "{":
            stack.append(offset)
        elif char == "}" and stack:
            pairs[stack.pop()] = offset
    return pairs


def if_block_ranges(masked: str, pairs: dict[int, int]) -> list[tuple[int, int, int]]:
    """Collect braced if bodies as (open, close, line)."""
    ranges = []
    for match in re.finditer(r"\bif\s*\(", masked):
        offset = match.end()
        depth = 1
        while offset < len(masked) and depth:
            if masked[offset] == "(":
                depth += 1
            elif masked[offset] == ")":
                depth -= 1
            offset += 1
        while offset < len(masked) and masked[offset].isspace():
            offset += 1
        if offset < len(masked) and masked[offset] == "{" and offset in pairs:
            line = masked.count("\n", 0, match.start()) + 1
            ranges.append((offset, pairs[offset], line))
    return ranges


def function_definitions(masked: str) -> list[tuple[str, int]]:
    """Find function definitions by matching a parameter list followed by '{'."""
    found: list[tuple[str, int]] = []
    seen: set[str] = set()
    for match in re.finditer(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", masked):
        name = match.group(1)
        if name in CONTROL_WORDS or name in seen:
            continue
        offset = match.end()
        depth = 1
        while offset < len(masked) and depth:
            if masked[offset] == "(":
                depth += 1
            elif masked[offset] == ")":
                depth -= 1
            offset += 1
        if depth:
            continue
        while offset < len(masked) and masked[offset].isspace():
            offset += 1
        if offset < len(masked) and masked[offset] == "{":
            line_start = masked.rfind("\n", 0, match.start()) + 1
            found.append((name, line_start))
            seen.add(name)
    return found


def main() -> int:
    total_functions = 0
    max_depth_overall = 0
    failures: list[str] = []

    for path in SOURCE_FILES:
        source = path.read_text(encoding="utf-8")
        masked = mask_comments_and_literals(source)
        pairs = brace_pairs(masked)
        ranges = if_block_ranges(masked, pairs)

        for opening, closing, line in ranges:
            depth = 1 + sum(
                1 for other_open, other_close, _ in ranges
                if other_open < opening and closing < other_close
            )
            max_depth_overall = max(max_depth_overall, depth)
            if depth > 2:
                failures.append(
                    f"{path.relative_to(ROOT)}:{line}: if 嵌套深度为 {depth}（上限 2）"
                )

        for name, line_start in function_definitions(masked):
            total_functions += 1
            prefix = source[:line_start].rstrip()
            doc_start = prefix.rfind("/**")
            doc_end = prefix.rfind("*/")
            if doc_start < 0 or doc_end < doc_start:
                line = source.count("\n", 0, line_start) + 1
                failures.append(
                    f"{path.relative_to(ROOT)}:{line}: 函数 {name} 缺少 Doxygen 函数注释"
                )

    if failures:
        print("结构检查失败：")
        for failure in failures:
            print(f" - {failure}")
        return 1

    print(f"结构检查通过：{len(SOURCE_FILES)} 个 C 源文件，"
          f"{total_functions} 个函数定义均有文档注释；"
          f"if 最大直接嵌套深度 = {max_depth_overall}（上限 2）。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
