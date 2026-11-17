#!/usr/bin/env python3
"""校验中文字库覆盖:main/*.c 里出现的每个汉字都必须在生成的字体里。

为什么值得单独测:UTF-8 源码和"构建成功"都不能证明屏幕上有字。缺字在设备上
只会显示成方块,而这类问题最容易在改文案时悄悄引入。测试同时带一个"必然缺字"
的反例,避免检查条件恒真。
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAIN = ROOT / "main"
SYMBOLS = ROOT / "assets" / "fonts" / "loop_font_16_symbols.txt"
SYMBOLS_24 = ROOT / "assets" / "fonts" / "loop_font_24_symbols.txt"
SYMBOLS_32 = ROOT / "assets" / "fonts" / "loop_font_32_symbols.txt"
KNOWN_MISSING = 0x9F98  # "龘":界面绝不该用到,用来证明检查不是恒真


def load_symbols(path: Path) -> set[int]:
    values: set[int] = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line.startswith("U+"):
            values.add(int(line[2:], 16))
    return values


def string_literals(text: str) -> list[str]:
    """只取 C 字符串字面量:注释里的汉字不需要渲染,不应参与覆盖检查。"""
    literals: list[str] = []
    i = 0
    length = len(text)
    while i < length:
        char = text[i]
        if char == "/" and i + 1 < length and text[i + 1] == "/":
            i = text.find("\n", i)
            if i < 0:
                break
        elif char == "/" and i + 1 < length and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            i = length if end < 0 else end + 2
        elif char == "'":
            i += 1
            while i < length and text[i] != "'":
                i += 2 if text[i] == "\\" else 1
            i += 1
        elif char == '"':
            i += 1
            chunk: list[str] = []
            while i < length and text[i] != '"':
                if text[i] == "\\" and i + 1 < length:
                    chunk.append(text[i])
                    chunk.append(text[i + 1])
                    i += 2
                    continue
                chunk.append(text[i])
                i += 1
            literals.append("".join(chunk))
            i += 1
        else:
            i += 1
    return literals


def used_codepoints() -> set[int]:
    used: set[int] = set()
    for path in sorted(MAIN.glob("*.c")):
        for literal in string_literals(path.read_text(encoding="utf-8")):
            for char in literal:
                if 0x3400 <= ord(char) <= 0x9FFF:
                    used.add(ord(char))
    return used


def main() -> int:
    errors: list[str] = []
    for path in (SYMBOLS, SYMBOLS_24, SYMBOLS_32):
        if not path.is_file():
            print(f"missing font symbol list: {path}", file=sys.stderr)
            return 1

    body = load_symbols(SYMBOLS)
    for path in (SYMBOLS_24, SYMBOLS_32):
        other = load_symbols(path)
        missing = body - other
        if missing:
            errors.append(
                f"{path.name} 比 16px 少 {len(missing)} 个码位: "
                + " ".join(f"U+{cp:04X}" for cp in sorted(missing)[:8])
            )

    used = used_codepoints()
    missing = sorted(used - body)
    if missing:
        errors.append(
            f"界面用了 {len(missing)} 个字体里没有的字: "
            + " ".join(f"U+{cp:04X}({chr(cp)})" for cp in missing)
        )

    if KNOWN_MISSING in body:
        errors.append(f"反例 U+{KNOWN_MISSING:04X} 不该出现在字库里,检查条件可能恒真")
    if not used:
        errors.append("没有从 main/*.c 里扫到任何汉字,扫描逻辑可能失效")

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(f"loop_font coverage: PASS ({len(used)} 个汉字,{len(body)} 个码位)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
