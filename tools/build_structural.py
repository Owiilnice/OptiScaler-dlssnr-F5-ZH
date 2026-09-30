#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
一次性工具：把「结构性改动」（字符串之外的代码/构建配置改动）固化成 dict/structural.json。

结构性改动不能靠字符串词典表达，必须写成「锚点 -> 替换」规则：
锚点是上游原文里的一段可识别代码，不是行号。上游把这段挪到哪都能命中；
上游把这段改掉了，apply_patch.py 会报错而不是静默跳过。

用法:
    python tools/build_structural.py <orig_dir> <zh_dir> [--out dict/structural.json]
"""
import argparse
import io
import json
import os
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

MC = "OptiScaler/menu/menu_common.cpp"
VCX = "OptiScaler/OptiScaler.vcxproj"
PS1 = "package_release.ps1"


def read_lines(root, rel):
    with io.open(os.path.join(root, rel), encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def slice_braced(lines, start):
    """从 start 行开始，按花括号配平切出完整语句块。

    返回 (文本, 结束行下标)。若紧跟 else / else if，一并纳入 —— 否则只替换 if 分支
    会留下孤立的 else，编译直接炸。
    """
    i = start
    while True:
        depth = 0
        seen = False
        end = None
        for i in range(i, len(lines)):
            depth += lines[i].count("{") - lines[i].count("}")
            if "{" in lines[i]:
                seen = True
            if seen and depth == 0:
                end = i
                break
        if end is None:
            raise SystemExit(f"花括号不配平，起始行 {start + 1}")
        j = end + 1
        while j < len(lines) and lines[j].strip() == "":
            j += 1
        if j < len(lines) and lines[j].strip().startswith("else"):
            i = j
            continue
        return "\n".join(lines[start : end + 1]), end


def find(lines, needle, start=0):
    for i in range(start, len(lines)):
        if needle in lines[i]:
            return i
    raise SystemExit(f"找不到锚点: {needle!r}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("orig")
    ap.add_argument("zh")
    ap.add_argument("--out", default="dict/structural.json")
    args = ap.parse_args()

    o = os.path.abspath(args.orig)
    z = os.path.abspath(args.zh)

    # ---- R3: menu_common.cpp 字体加载块 ------------------------------------
    ol = read_lines(o, MC)
    zl = read_lines(z, MC)

    anchor_i = find(ol, "if (Config::Instance()->TTFFontPath.has_value())",
                    find(ol, "fontSize = Config::Instance()->FontSize.value();"))
    before3, _ = slice_braced(ol, anchor_i)

    zstart = find(zl, "// Chinese localization: the built-in Hack font carries no CJK glyphs")
    zend = find(zl, "&fontConfig, glyphRanges);", zstart) + 1
    after3 = "\n".join(zl[zstart : zend + 1])

    rules = [
        {
            "id": "menu_common_include_fstream",
            "file": MC,
            "desc": "字体改为从文件读取，需要 <fstream>",
            "mode": "insert_after",
            "anchor": "#include <cfloat>",
            "after": "#include <fstream>",
            "marker": "#include <fstream>",
        },
        {
            "id": "menu_common_include_glyphranges",
            "file": MC,
            "desc": "引入 CJK 字形范围表",
            "mode": "insert_after",
            "anchor": '#include "font/Hack_Compressed.h"',
            "after": '#include "font/ChineseGlyphRanges.h"',
            "marker": "font/ChineseGlyphRanges.h",
        },
        {
            "id": "menu_common_cjk_font_block",
            "file": MC,
            "desc": "字体加载改为 CJK 候选链（内置 Hack 无汉字字形，必须外挂字体）",
            "mode": "replace",
            "before": before3,
            "after": after3,
            "marker": "OptiScalerLocalization::ChineseGlyphRanges",
        },
        {
            "id": "vcxproj_utf8",
            "file": VCX,
            "desc": "MSVC 默认按系统 ANSI 代码页解析无 BOM 源码，中文源码必须 /utf-8",
            "mode": "replace_all",
            "before": "<AdditionalOptions>/w34996 %(AdditionalOptions)</AdditionalOptions>",
            "after": "<AdditionalOptions>/utf-8 /w34996 %(AdditionalOptions)</AdditionalOptions>",
            "marker": "/utf-8",
        },
        {
            "id": "vcxproj_font_copy",
            "file": VCX,
            "desc": "Release 后置事件把 font\\ 拷进输出目录",
            "mode": "insert_after",
            "anchor": 'copy "$(SolutionDir)external\\directx_agility_sdk\\LICENSE.txt" '
                      "$(SolutionDir)x64\\Release\\a\\Licenses\\DirectX_LICENSE.txt /Y",
            "after": "md $(SolutionDir)x64\\Release\\a\\font\n"
                     "copy $(SolutionDir)font\\wqy-microhei.ttc $(SolutionDir)x64\\Release\\a\\font\\ /Y\n"
                     "copy $(SolutionDir)font\\README.txt $(SolutionDir)x64\\Release\\a\\font\\ /Y",
            "marker": "x64\\Release\\a\\font",
        },
        {
            "id": "package_release_font_copy",
            "file": PS1,
            "desc": "打包时带上 font\\",
            "mode": "insert_after",
            "anchor": 'Copy-Item -LiteralPath "$root\\docs" -Destination "$stage\\docs" -Recurse -Force',
            "after": "\n"
                     "# Simplified Chinese localization: the bundled CJK font. OptiScaler looks for it at\n"
                     "# <game>\\font\\wqy-microhei.ttc (next to the DLL) when the UI is Chinese.\n"
                     'Copy-Item -LiteralPath "$root\\font" -Destination "$stage\\font" -Recurse -Force',
            "marker": "$stage\\font",
        },
    ]

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with io.open(args.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(rules, f, ensure_ascii=False, indent=1)

    print(f"结构规则 {len(rules)} 条 -> {args.out}")
    for r in rules:
        n = len(r.get("before", r.get("anchor", "")).splitlines())
        print(f"  {r['id']:34s} {r['mode']:13s} {r['file']}  ({n} 行锚点)")


if __name__ == "__main__":
    main()
