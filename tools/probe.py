#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""排查工具：列出某个英文字面量在「原文 / 参考汉化版」里的全部出现处，逐处对照。

用途：verify.py 报出差异后，用这个工具看清同一英文串在哪些上下文里该译、哪些不该译，
      据此决定写进 dict/overrides.json（按上下文分派）还是 dict/protected.json（永不翻译）。

用法:
    python tools/probe.py <orig_dir> <ref_dir> "<字面量>" [...]
"""
import io
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"))
from apply_patch import scan_groups, line_of  # noqa: E402

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def occurrences(root, needle):
    hits = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in (".git", "external", "images")]
        for fn in filenames:
            if not fn.lower().endswith((".cpp", ".h", ".inl", ".hpp")):
                continue
            p = os.path.join(dirpath, fn)
            rel = os.path.relpath(p, root).replace("\\", "/")
            txt = io.open(p, encoding="utf-8", errors="replace").read()
            for value, s, e in scan_groups(txt):
                if value == needle:
                    hits.append((rel, line_of(txt, s).strip()))
    return hits


def main():
    orig, ref = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
    for needle in sys.argv[3:]:
        print(f"===== {needle!r} =====")
        print("  原文:")
        for rel, l in occurrences(orig, needle):
            print(f"    {rel}: {l[:130]}")
        print("  参考汉化版:")
        for rel, l in occurrences(ref, needle):
            print(f"    {rel}: {l[:130]}")
        print()


if __name__ == "__main__":
    main()
