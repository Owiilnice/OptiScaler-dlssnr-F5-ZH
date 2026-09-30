#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
汉化重放器：把汉化资产套用到任意版本的 OptiScaler-F5-DLSSNR-Multipass 源码上。

用法:
    python scripts/apply_patch.py <源码根目录> [--check] [--report out.json]

三条流水线，依次执行：
    1. 结构规则  dict/structural.json   —— 代码/构建配置改动，锚点是原文片段，不是行号
    2. 字面量改写 dict/strings.json + dict/overrides.json —— 锚点是英文原文
    3. 覆盖文件  overlay/               —— 汉化独有的新文件（字形范围表、字体）

设计要点：
  * 幂等。重复跑不会二次替换（已含汉字的字面量直接跳过，结构规则看 marker）。
  * 锚点缺失 = 报错退出，不是静默跳过。上游改了同一处代码时 CI 必须红，而不是出一个半汉化的包。
  * 只改 dict/targets.json 圈定的文件。
"""
import argparse
import glob
import io
import json
import os
import re
import shutil
import sys
import textwrap

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DICT = os.path.join(ROOT, "dict")
OVERLAY = os.path.join(ROOT, "overlay")


# ---------------------------------------------------------------- 字面量扫描

def scan_groups(text):
    """切出所有「字面量组」，返回 [(value, start, end)]。

    相邻字面量（C 的 `"a" "b"` 拼接语法，可跨行）合成一个逻辑串，
    这样多行 HelpMarker 折成一行或拆成多行都能正确命中。
    """
    groups = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j + 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
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
            gstart = i
            parts = []
            while True:
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
                gend = i + 1
                i += 1
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


def line_of(text, pos):
    s = text.rfind("\n", 0, pos) + 1
    e = text.find("\n", pos)
    return text[s : len(text) if e < 0 else e]


def is_include_line(line, pos_in_line):
    return re.match(r"\s*#\s*include\b", line[:pos_in_line]) is not None


# ---------------------------------------------------------------- 文件读写

def read_text(path):
    with io.open(path, "rb") as f:
        raw = f.read()
    bom = raw.startswith(b"\xef\xbb\xbf")
    if bom:
        raw = raw[3:]
    crlf = raw.count(b"\r\n") > raw.count(b"\n") / 2
    text = raw.decode("utf-8", errors="replace")
    if crlf:
        text = text.replace("\r\n", "\n")
    return text, bom, crlf


def write_text(path, text, bom, crlf):
    if crlf:
        text = text.replace("\n", "\r\n")
    data = text.encode("utf-8")
    if bom:
        data = b"\xef\xbb\xbf" + data
    with io.open(path, "wb") as f:
        f.write(data)


# ---------------------------------------------------------------- 结构规则

def flex_regex(block):
    """把代码块编译成「空白不敏感」的正则：换行、缩进怎么变都能命中。"""
    parts = [re.escape(p) for p in re.split(r"\s+", block.strip())]
    return re.compile(r"\s+".join(parts))


def apply_structural(text, rule, rep):
    mode = rule["mode"]
    marker = rule.get("marker")

    if mode in ("replace", "replace_all"):
        if marker and marker in text:
            rep["skipped"].append(rule["id"])
            return text
        before = rule["before"]
        if mode == "replace_all":
            cnt = text.count(before)
            if cnt == 0:
                rep["failed"].append({"id": rule["id"], "why": "锚点未命中"})
                return text
            rep["applied"].append({"id": rule["id"], "hits": cnt})
            return text.replace(before, rule["after"])
        m = flex_regex(before).search(text)
        if not m:
            rep["failed"].append({"id": rule["id"], "why": "锚点未命中"})
            return text
        indent = re.match(r"[ \t]*", text[text.rfind("\n", 0, m.start()) + 1 :]).group(0)
        new = textwrap.indent(textwrap.dedent(rule["after"]), indent)
        rep["applied"].append({"id": rule["id"], "hits": 1})
        return text[: m.start()] + new + text[m.end() :]

    if mode == "insert_after":
        if marker and marker in text:
            rep["skipped"].append(rule["id"])
            return text
        anchor = rule["anchor"]
        lines = text.split("\n")
        idx = next((i for i, l in enumerate(lines) if anchor in l), None)
        if idx is None:
            rep["failed"].append({"id": rule["id"], "why": "锚点未命中"})
            return text
        indent = re.match(r"[ \t]*", lines[idx]).group(0)
        ins = [(indent + l if l.strip() else l) for l in rule["after"].split("\n")]
        rep["applied"].append({"id": rule["id"], "hits": 1})
        return "\n".join(lines[: idx + 1] + ins + lines[idx + 1 :])

    rep["failed"].append({"id": rule["id"], "why": f"未知 mode: {mode}"})
    return text


# ---------------------------------------------------------------- 字面量改写

def rewrite_literals(text, rel, gdict, ovr, rep):
    edits = []
    # "*" 是全局规则，文件级规则覆盖同名键
    file_ovr = dict(ovr.get("*", {}))
    file_ovr.update(ovr.get(rel, {}))
    for value, s, e in scan_groups(text):
        if has_cjk(value):
            continue
        line = line_of(text, s)
        if is_include_line(line, s - (text.rfind("\n", 0, s) + 1)):
            continue

        zh = None
        rules = file_ovr.get(value)
        if rules is not None:
            for r in rules:
                ctx = r.get("ctx")
                if ctx is None or ctx in line:
                    zh = r["zh"]
                    rep["override_hits"].append(f"{rel}: {value!r} -> {zh!r}")
                    rep["override_keys"].add(value)
                    break
        elif value in gdict:
            zh = gdict[value]
            rep["dict_hits"][value] = rep["dict_hits"].get(value, 0) + 1

        if zh is None or zh == value:
            continue
        edits.append((s, e, '"' + zh + '"'))

    for s, e, new in reversed(edits):
        text = text[:s] + new + text[e:]
    rep["literal_files"][rel] = len(edits)
    return text


# ---------------------------------------------------------------- 主流程

def expand_targets(src, cfg):
    """按 globs 展开目标文件。

    用 glob 的 recursive=True —— `**` 能匹配零层目录，所以
    `OptiScaler/menu/**/*.cpp` 既覆盖 menu/menu_common.cpp 也覆盖 menu/input/*.cpp。
    fnmatch 做不到这点（它的 `**` 退化成普通 `*`，中间那个 `/` 会把同层文件漏掉）。
    """
    out = set()
    for g in cfg["globs"]:
        for p in glob.glob(os.path.join(src, g), recursive=True):
            if os.path.isfile(p):
                out.add(os.path.relpath(p, src).replace("\\", "/"))
    for e in cfg.get("exclude", []):
        out.discard(e)
    return sorted(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("--check", action="store_true", help="只报告不写盘")
    ap.add_argument("--report", default=None)
    args = ap.parse_args()

    src = os.path.abspath(args.source)
    if not os.path.isdir(src):
        raise SystemExit(f"源码目录不存在: {src}")

    gdict = json.load(io.open(os.path.join(DICT, "strings.json"), encoding="utf-8"))
    ovr = json.load(io.open(os.path.join(DICT, "overrides.json"), encoding="utf-8"))
    # 下划线开头的键是文档注释，不是规则
    for k in [k for k in ovr if k.startswith("_")]:
        ovr.pop(k)
    struct = json.load(io.open(os.path.join(DICT, "structural.json"), encoding="utf-8"))
    cfg = json.load(io.open(os.path.join(DICT, "targets.json"), encoding="utf-8"))

    rep = {"applied": [], "skipped": [], "failed": [], "override_hits": [],
           "override_keys": set(), "dict_hits": {}, "literal_files": {}, "overlay": []}

    # 1) 结构规则
    by_file = {}
    for r in struct:
        by_file.setdefault(r["file"], []).append(r)
    for rel, rules in sorted(by_file.items()):
        path = os.path.join(src, rel)
        if not os.path.exists(path):
            for r in rules:
                rep["failed"].append({"id": r["id"], "why": f"目标文件不存在: {rel}"})
            continue
        text, bom, crlf = read_text(path)
        for r in rules:
            text = apply_structural(text, r, rep)
        if not args.check:
            write_text(path, text, bom, crlf)

    # 2) 字面量改写
    targets = expand_targets(src, cfg)
    for rel in targets:
        path = os.path.join(src, rel)
        text, bom, crlf = read_text(path)
        new = rewrite_literals(text, rel, gdict, ovr, rep)
        if new != text and not args.check:
            write_text(path, new, bom, crlf)

    # 3) 覆盖文件
    for dirpath, dirnames, filenames in os.walk(OVERLAY):
        for fn in filenames:
            s = os.path.join(dirpath, fn)
            rel = os.path.relpath(s, OVERLAY)
            d = os.path.join(src, rel)
            rep["overlay"].append(rel.replace("\\", "/"))
            if not args.check:
                os.makedirs(os.path.dirname(d), exist_ok=True)
                shutil.copy2(s, d)

    # 4) 统计：词典里哪些条目一次都没命中
    #    被 overrides 接管的条目（Auto / yes / no / Balanced / Neural Rendering 这类
    #    上下文相关的串）也算命中 —— 它们只是没走全局词典这条路。
    used = set(rep["dict_hits"]) | set(rep["override_keys"])
    unused = sorted(set(gdict) - used)
    rep["unused_dict_entries"] = unused
    rep["override_keys"] = sorted(rep["override_keys"])

    total_lit = sum(rep["literal_files"].values())
    print(f"目标文件 {len(targets)} 个，字面量替换 {total_lit} 处")
    print(f"结构规则：应用 {len(rep['applied'])}，已存在跳过 {len(rep['skipped'])}，失败 {len(rep['failed'])}")
    print(f"覆盖文件 {len(rep['overlay'])} 个")
    print(f"词典 {len(gdict)} 条，命中 {len(used & set(gdict))}，未命中 {len(unused)} 条")
    if unused:
        print("  未命中前 10 条（上游可能已删除该串）:")
        for u in unused[:10]:
            print(f"    {u[:90]!r}")

    if args.report:
        with io.open(args.report, "w", encoding="utf-8", newline="\n") as f:
            json.dump(rep, f, ensure_ascii=False, indent=1)
        print(f"报告 -> {args.report}")

    if rep["failed"]:
        print("\n!! 结构锚点未命中，汉化不完整，必须人工修复：")
        for x in rep["failed"]:
            print(f"   {x['id']}: {x['why']}")
        sys.exit(2)


if __name__ == "__main__":
    main()
