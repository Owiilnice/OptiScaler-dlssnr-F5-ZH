#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
一次性工具：从「纯净上游源码」与「已汉化源码」的对比中抽取 en -> zh 词典。

用法:
    python tools/extract_dict.py <orig_dir> <zh_dir> [--out dict/strings.json] [--report tools/_report.txt]

原理:
    用 C/C++ 字符串字面量扫描器把两侧源码切成「字面量组」——相邻的多个字面量
    （C 的相邻字面量拼接语法，`"a" "b"`）合成一个逻辑串。然后对逻辑串序列做 diff。
    这样多行 HelpMarker 折叠成单行、或单行拆成多行，都能正确配对。

    产出的 en -> zh 就是汉化唯一需要长期维护的资产：锚点是「英文原文」而不是行号，
    所以上游怎么改代码位置都不影响重放。
"""
import argparse
import difflib
import io
import json
import os
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


def scan_groups(text):
    """把源码切成字面量组。

    返回 [(value, start, end)]，value 是相邻字面量拼接后的逻辑串。
    跳过注释、字符字面量、预处理指令里的 include 路径。
    """
    groups = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        # 行注释
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j + 1
            continue
        # 块注释
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        # 字符字面量
        if c == "'":
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == "'":
                    i += 1
                    break
                i += 1
            continue
        if c == '"':
            # 收集一组相邻字面量
            gstart = i
            parts = []
            gend = i
            while True:
                assert text[i] == '"'
                i += 1
                buf = []
                while i < n:
                    if text[i] == "\\":
                        buf.append(text[i : i + 2])
                        i += 2
                        continue
                    if text[i] == '"':
                        break
                    buf.append(text[i])
                    i += 1
                parts.append("".join(buf))
                gend = i + 1  # 含收尾引号
                i += 1
                # 前瞻：只跳过空白/换行，若仍是字面量则并入同组
                j = i
                while j < n and text[j] in " \t\r\n":
                    j += 1
                if j < n and text[j] == '"':
                    i = j
                    continue
                break
            groups.append(("".join(parts), gstart, gend))
            continue
        i += 1
    return groups


def has_cjk(s):
    return any("\u4e00" <= ch <= "\u9fff" for ch in s)


# ---------------------------------------------------------------------------
# 人工裁决表。
# 抽取器只能按「位置配对」，下面两类它判不了，必须显式写死，否则重放会出错：
#
# CONFLICT_KEYS  同一英文串在不同上下文译法不同，全局词典容纳不下。
#                从全局词典剔除，改由 dict/overrides.json 按「所在行上下文」分派。
# EXTRA_PAIRS    译法里没有汉字（全角括号、去复数等），抽取器的 has_cjk 过滤会误杀。
# ---------------------------------------------------------------------------
CONFLICT_KEYS = {
    "Motion",  # 按钮标签 = 运动；AddResourceBarrier 调试名 = 运动矢量
    "ON",      # 状态徽标，原文既有「开」也有「开启」
    "OFF",     # 状态徽标，原文既有「关」也有「关闭」
    "s",       # 复数后缀 "point%s"，中文无复数，整串去掉
}

EXTRA_PAIRS = {
    "NVIDIA (FP8)": "NVIDIA（FP8）",
    "(DXVK) ": "（DXVK） ",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("orig")
    ap.add_argument("zh")
    ap.add_argument("--out", default="dict/strings.json")
    ap.add_argument("--report", default="tools/_report.txt")
    args = ap.parse_args()

    orig_root = os.path.abspath(args.orig)
    zh_root = os.path.abspath(args.zh)

    targets = []
    for dirpath, dirnames, filenames in os.walk(zh_root):
        dirnames[:] = [d for d in dirnames if d not in (".git", "external", "images")]
        for fn in filenames:
            if not fn.lower().endswith((".cpp", ".h", ".inl", ".hpp")):
                continue
            rel = os.path.relpath(os.path.join(dirpath, fn), zh_root).replace("\\", "/")
            if os.path.exists(os.path.join(orig_root, rel)):
                targets.append(rel)
    targets.sort()

    pairs = {}       # en -> zh
    sources = {}     # en -> {files}
    conflicts = {}   # en -> {zh: [locs]}
    odd = []         # 非中文改动（需要人工确认的边角）
    stats = {}

    for rel in targets:
        with io.open(os.path.join(orig_root, rel), encoding="utf-8", errors="replace") as f:
            a_txt = f.read()
        with io.open(os.path.join(zh_root, rel), encoding="utf-8", errors="replace") as f:
            b_txt = f.read()
        if a_txt == b_txt:
            continue

        ga = scan_groups(a_txt)
        gb = scan_groups(b_txt)
        va = [g[0] for g in ga]
        vb = [g[0] for g in gb]

        n_pure = n_odd = 0
        sm = difflib.SequenceMatcher(None, va, vb, autojunk=False)
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag == "equal":
                continue
            if tag == "replace" and (i2 - i1) == (j2 - j1):
                for k in range(i2 - i1):
                    en, zh = va[i1 + k], vb[j1 + k]
                    if en == zh:
                        continue
                    if has_cjk(zh):
                        conflicts.setdefault(en, {}).setdefault(zh, []).append(rel)
                        pairs[en] = zh
                        sources.setdefault(en, set()).add(rel)
                        n_pure += 1
                    else:
                        n_odd += 1
                        odd.append((rel, en, zh))
            else:
                odd.append((rel, f"<{tag} {i2 - i1}->{j2 - j1}>",
                            " || ".join(va[i1:i2][:3])[:200] + " ==> " + " || ".join(vb[j1:j2][:3])[:200]))
                n_odd += max(i2 - i1, j2 - j1)
        stats[rel] = (n_pure, n_odd)

    real_conflicts = {en: v for en, v in conflicts.items() if len(v) > 1}

    # 应用人工裁决表
    for k in CONFLICT_KEYS:
        pairs.pop(k, None)
        sources.pop(k, None)
        conflicts.pop(k, None)
    for en, zh in EXTRA_PAIRS.items():
        pairs[en] = zh

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with io.open(args.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump({k: pairs[k] for k in sorted(pairs)}, f, ensure_ascii=False, indent=1)

    with io.open(args.report, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"词典条目: {len(pairs)}\n")
        f.write(f"冲突（同英文多译法）: {len(real_conflicts)}\n")
        f.write(f"未归类改动: {len(odd)}\n")
        f.write(f"人工裁决剔除 {len(CONFLICT_KEYS)} 条（转 overrides.json）: {sorted(CONFLICT_KEYS)}\n")
        f.write(f"人工裁决补入 {len(EXTRA_PAIRS)} 条（无汉字译法）: {sorted(EXTRA_PAIRS)}\n\n")
        f.write("== 逐文件 (配对数, 未归类) ==\n")
        for rel in sorted(stats):
            p, o = stats[rel]
            f.write(f"  {p:5d} {o:5d}  {rel}\n")
        f.write(f"\n== 冲突 {len(real_conflicts)} 组（已剔除者不再列出）==\n")
        for en, v in sorted(real_conflicts.items()):
            if en in CONFLICT_KEYS:
                continue
            f.write(f"  {en!r}\n")
            for zh, locs in v.items():
                f.write(f"      {zh!r}  <- {', '.join(sorted(set(locs))[:4])}\n")
        f.write(f"\n== 未归类改动 {len(odd)} 处 ==\n")
        for rel, en, zh in odd:
            f.write(f"  {rel}\n      - {en!r}\n      + {zh!r}\n")

    print(f"词典 {len(pairs)} 条 -> {args.out}")
    print(f"冲突 {len(real_conflicts)} 组，未归类 {len(odd)} 处 -> {args.report}")
    for rel in sorted(stats):
        p, o = stats[rel]
        print(f"  {p:5d} pair  {o:5d} odd   {rel}")


if __name__ == "__main__":
    main()
