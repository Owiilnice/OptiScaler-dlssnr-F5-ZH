#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
汉化校验：拿「重放结果」和「已知可用的参考汉化版」对账。

用法:
    python scripts/verify.py <重放后的源码根目录> <参考汉化版源码根目录> [--report out.txt]

比对的是「字面量组的逻辑串序列」，不是文本 diff —— 这样多行字面量折叠成一行
（`"a\n" "b"` 变 `"a\nb"`，C 的相邻字面量拼接，语义完全等价）不会被误报成差异，
只有真正译法不同、或该译没译的地方才会浮出来。

退出码 0 = 一致；1 = 有差异（需人工判定是回归还是改进）。
"""
import argparse
import io
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from apply_patch import scan_groups, has_cjk  # noqa: E402

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

DICT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "dict")

# 占位符必须原样保留，否则格式化会崩。三个坑都踩过：
#  1. "100% or above" 里的百分号会被当成 %o —— 用 (?<![\d%]) 排除「数字后的百分号」
#  2. "%llu" 的 ll 长度修饰符 —— 必须显式列在 flags 之后
#  3. "frames%s" 里 %s 紧跟在字母后是合法的 —— 所以不能简单用 \w 做否定后顾，
#     那样会把真占位符也滤掉
PLACEHOLDER = re.compile(
    r"(?<![\d%])(?:%%|%[-+#0]*[\d.*]*(?:hh|h|ll|l|j|z|t|L)?[diufFeEgGxXoscpaAn])|\{[^{}]*\}"
)


def placeholders(s):
    return sorted(PLACEHOLDER.findall(s))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("ref")
    ap.add_argument("--report", default=None)
    args = ap.parse_args()

    src = os.path.abspath(args.src)
    ref = os.path.abspath(args.ref)

    diffs = []
    missing = []
    files = 0
    for dirpath, dirnames, filenames in os.walk(ref):
        dirnames[:] = [d for d in dirnames if d not in (".git", "external", "images")]
        for fn in filenames:
            if not fn.lower().endswith((".cpp", ".h", ".inl", ".hpp", ".vcxproj", ".ps1")):
                continue
            rp = os.path.join(dirpath, fn)
            rel = os.path.relpath(rp, ref).replace("\\", "/")
            sp = os.path.join(src, rel)
            if not os.path.exists(sp):
                # 参考版有、被比对目录没拷过来的文件（测试、脚本等）不算差异
                missing.append(rel)
                continue
            files += 1
            a = [g[0] for g in scan_groups(io.open(rp, encoding="utf-8", errors="replace").read())]
            b = [g[0] for g in scan_groups(io.open(sp, encoding="utf-8", errors="replace").read())]
            if a == b:
                continue
            # 只报「参考版有、重放版不同」的位置
            import difflib
            sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
            for tag, i1, i2, j1, j2 in sm.get_opcodes():
                if tag == "equal":
                    continue
                for k in range(max(i2 - i1, j2 - j1)):
                    rv = a[i1 + k] if i1 + k < i2 else ""
                    sv = b[j1 + k] if j1 + k < j2 else ""
                    if rv == sv:
                        continue
                    diffs.append((rel, tag, rv, sv))

    # 词典自检：占位符一致性
    gdict = json.load(io.open(os.path.join(DICT, "strings.json"), encoding="utf-8"))
    ph_bad = []
    for en, zh in gdict.items():
        if placeholders(en) != placeholders(zh):
            ph_bad.append((en, zh))

    # 已知且已接受的差异：从告警里摘出去
    acc_path = os.path.join(DICT, "accepted_diffs.json")
    accepted = {}
    if os.path.exists(acc_path):
        for e in json.load(io.open(acc_path, encoding="utf-8"))["entries"]:
            accepted[(e["file"], e["ref"], e["ours"])] = e
    known, fresh = [], []
    for d in diffs:
        key = (d[0], d[2], d[3])
        if key in accepted:
            known.append(d)
        else:
            fresh.append(d)

    lines = []
    lines.append(f"比对文件 {files} 个（另有 {len(missing)} 个文件只存在于参考版，未纳入比对）")
    lines.append(f"与参考版的差异 {len(diffs)} 处：已知已接受 {len(known)}，新增 {len(fresh)}")
    lines.append(f"词典占位符不一致 {len(ph_bad)} 条")
    lines.append("")
    if fresh:
        lines.append("== 新增差异（需判定是回归还是改进）==")
        lines.append("  确认没问题就把条目加进 dict/accepted_diffs.json，否则修 dict/")
        for rel, tag, rv, sv in fresh:
            lines.append(f"  {rel}  [{tag}]")
            lines.append(f"      参考: {rv!r}")
            lines.append(f"      重放: {sv!r}")
    if known:
        lines.append("")
        lines.append("== 已知已接受 ==")
        seen = {}
        for rel, tag, rv, sv in known:
            seen.setdefault((rel, rv, sv), 0)
            seen[(rel, rv, sv)] += 1
        for (rel, rv, sv), n in sorted(seen.items()):
            why = accepted[(rel, rv, sv)]["why"]
            lines.append(f"  x{n}  {rel}: {rv!r} -> {sv!r}")
            lines.append(f"      {why}")
    if ph_bad:
        lines.append("")
        lines.append("== 词典占位符不一致（会崩格式化，必须修）==")
        for en, zh in ph_bad:
            lines.append(f"  {en!r} -> {zh!r}")
            lines.append(f"      en: {placeholders(en)}")
            lines.append(f"      zh: {placeholders(zh)}")

    out = "\n".join(lines)
    print(out)
    if args.report:
        io.open(args.report, "w", encoding="utf-8", newline="\n").write(out + "\n")

    sys.exit(1 if (fresh or ph_bad) else 0)


if __name__ == "__main__":
    main()
