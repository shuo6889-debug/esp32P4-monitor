#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check-code-identical.py —— 校验 annotated/ 下的注释副本与原工程代码完全一致

原理
----
把「原文件」和「注释副本」都用同样的规则剔除注释，
再逐行比较（忽略每行首尾空白与空行）。

 · 剔除的内容 = 本次新增的中文注释
 · 比较的内容 = 真正的代码

若所有文件都输出 [OK]，说明注释副本里的代码与原工程逐字一致，
不存在「抄错了代码」的风险，可以放心当作原代码阅读。

用法
----
    python check-code-identical.py
"""

import os
import sys

# (注释风格, 原工程相对路径, 副本相对路径)
#   c     = C/C++ 风格：// 与 /* */
#   hash  = # 到行尾（CMake / Kconfig / YAML）
#   slash = // 与 /* */（JSONC，VS Code 允许注释）
#   md    = 剔除以 > 开头的引用行（用于给 Markdown 加注解）
PAIRS = [
    ("c",     "main/main.cpp",          "main/main.cpp"),
    ("c",     "main/example_config.h",  "main/example_config.h"),
    ("hash",  "main/CMakeLists.txt",    "main/CMakeLists.txt"),
    ("hash",  "main/idf_component.yml", "main/idf_component.yml"),
    ("hash",  "CMakeLists.txt",         "CMakeLists.txt"),
    ("hash",  "sdkconfig.defaults",     "sdkconfig.defaults"),
    ("hash",  ".clangd",                ".clangd"),
    ("slash", ".vscode/settings.json",  ".vscode/settings.json"),
    ("slash", ".vscode/launch.json",    ".vscode/launch.json"),
    ("md",    "README.md",              "README.md"),
]


def strip_c_comments(text):
    """剔除 // 行注释与 /* */ 块注释，但不动字符串/字符字面量内部的字符。"""
    out = []
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        # 进入字符串或字符字面量：原样拷贝，直到配对的引号结束
        if ch in ('"', "'"):
            quote = ch
            out.append(ch)
            i += 1
            while i < n:
                c = text[i]
                out.append(c)
                if c == "\\":                     # 转义序列：连下一个字符一起拷贝
                    if i + 1 < n:
                        out.append(text[i + 1])
                        i += 2
                        continue
                    i += 1
                    continue
                i += 1
                if c == quote:
                    break
            continue
        # // 行注释
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        # /* */ 块注释
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def strip_hash_comments(text):
    """剔除 # 到行尾的注释（用于 CMake / Kconfig / YAML）。"""
    lines = []
    for line in text.splitlines():
        idx = line.find("#")
        lines.append(line if idx < 0 else line[:idx])
    return "\n".join(lines)


def strip_md_notes(text):
    """剔除 Markdown 注解行（以 > 引用块形式加入的说明），保留原文。"""
    lines = []
    for line in text.splitlines():
        lines.append("" if line.lstrip().startswith(">") else line)
    return "\n".join(lines)


def to_code_lines(text, style):
    """剔除注释 → 去掉空白 → 丢掉空行，得到「纯代码行」列表。"""
    if style == "hash":
        text = strip_hash_comments(text)
    elif style == "md":
        text = strip_md_notes(text)
    else:
        text = strip_c_comments(text)
    return [ln.strip() for ln in text.splitlines() if ln.strip()]


def read_text(path):
    with open(path, "r", encoding="utf-8") as fh:
        return fh.read()


def main():
    here = os.path.dirname(os.path.abspath(__file__))   # .../annotated
    project = os.path.dirname(here)                     # 工程根目录
    failures = 0

    print("工程根目录: %s" % project)
    print("注释副本:   %s" % here)
    print("-" * 72)

    for style, orig_rel, copy_rel in PAIRS:
        orig_path = os.path.join(project, orig_rel)
        copy_path = os.path.join(here, copy_rel)

        if not os.path.isfile(orig_path):
            print("[跳过] %-28s 原文件不存在" % orig_rel)
            continue
        if not os.path.isfile(copy_path):
            print("[缺失] %-28s 副本不存在" % copy_rel)
            failures += 1
            continue

        orig_lines = to_code_lines(read_text(orig_path), style)
        copy_lines = to_code_lines(read_text(copy_path), style)

        if orig_lines == copy_lines:
            print("[OK]   %-28s 代码一致（原 %d 行）" % (orig_rel, len(orig_lines)))
            continue

        failures += 1
        print("[差异] %-28s 代码不一致！" % orig_rel)
        # 找出第一处不同，方便定位
        limit = min(len(orig_lines), len(copy_lines))
        for i in range(limit):
            if orig_lines[i] != copy_lines[i]:
                print("        第 %d 行代码不同：" % (i + 1))
                print("          原文件: %s" % orig_lines[i])
                print("          副本  : %s" % copy_lines[i])
                break
        else:
            print("        行数不同：原 %d 行，副本 %d 行" % (len(orig_lines), len(copy_lines)))
            if len(orig_lines) > limit:
                print("          原文件多出: %s" % orig_lines[limit])
            else:
                print("          副本多出:   %s" % copy_lines[limit])

    print("-" * 72)
    if failures == 0:
        print("结论：全部通过 —— 注释副本中的代码与原工程完全一致。")
        return 0
    print("结论：有 %d 个文件不一致，请检查。" % failures)
    return 1


if __name__ == "__main__":
    sys.exit(main())
