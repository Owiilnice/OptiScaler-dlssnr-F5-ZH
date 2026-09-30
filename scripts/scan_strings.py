#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
扫描上游源码里「界面上会出现、但词典还没收录」的英文字符串。

用法:
    python scripts/scan_strings.py <源码根目录> [--out pending.json] [--markdown pending.md]

判定思路：不猜语义，只看它出现在哪个调用里。只有落在 ImGui 控件、帮助标记、
状态提示这些「给人看的」调用参数位置上的字面量才算候选；日志、断言、测试、
include 路径、ini 键名一律排除。宁可少报也不误报 —— 误报会让翻译器把
功能串改成中文，那是会出 bug 的。
"""
import argparse
import glob
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

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DICT = os.path.join(ROOT, "dict")

# 出现在这些调用里的字面量是给人看的
UI_CALLS = re.compile(
    r"\b(?:ImGui::(?:Text|TextWrapped|TextDisabled|TextColored|TextUnformatted|Button|SmallButton|Checkbox|"
    r"Selectable|MenuItem|RadioButton|BeginCombo|CollapsingHeader|SliderFloat|SliderInt|InputText|InputFloat|"
    r"BeginTabItem|Begin|BeginChild|PushID|TreeNode|TreeNodeEx|LabelText|BulletText)"
    r"|ShowHelpMarker|HelpMarker|ScopedCollapsingHeader|Fail|Toast|Keybind|StrFmt|AddResourceBarrier"
    r"|AddDLSS\w*|AddFSR\w*|AddXe\w*|AddOpti\w*|setTitle|setContent)\b"
)

# 出现在这些调用里的字面量是给机器/日志看的，永远不翻
NON_UI_CALLS = re.compile(
    r"\b(?:LOG_\w+|spdlog|IM_ASSERT|assert|printf|fprintf|snprintf|sprintf|catch|throw|std::runtime_error)\b"
)

# 明显不是界面文案的东西
NOT_UI_VALUE = re.compile(
    r"(?:^##|^%|^\\\\|\.dll$|\.ini$|\.ttf$|\.ttc$|\.json$|\.log$|\.txt$|^\.|^[0-9.]+$)"
)

# 单个词（无空格无冒号）的额外判定：
#   "Upscaler" / "Framerate" / "Auto" 这类首字母大写 + 全小写的词是界面标签，保留
#   "DXVK" / "nvngx" / "mv_resource" / "kNames" 这类不是，丢掉
BARE_WORD_OK = re.compile(r"^[A-Z][a-z]{2,}$")


def looks_like_ui(value):
    if NOT_UI_VALUE.search(value):
        return False
    if " " not in value and ":" not in value and not BARE_WORD_OK.match(value):
        return False
    return True


def statement_start(text, pos):
    """回退到所在语句的开头，用来判断这个字面量属于哪个调用。"""
    s = max(text.rfind(";", 0, pos), text.rfind("{", 0, pos), text.rfind("}", 0, pos))
    return s + 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("--out", default="pending.json")
    ap.add_argument("--markdown", default=None)
    args = ap.parse_args()

    src = os.path.abspath(args.source)
    cfg = json.load(io.open(os.path.join(DICT, "targets.json"), encoding="utf-8"))
    gdict = json.load(io.open(os.path.join(DICT, "strings.json"), encoding="utf-8"))
    ovr = json.load(io.open(os.path.join(DICT, "overrides.json"), encoding="utf-8"))
    known = set(gdict)
    for f, m in ovr.items():
        if not f.startswith("_"):
            known |= set(m)

    files = set()
    for g in cfg["globs"]:
        for p in glob.glob(os.path.join(src, g), recursive=True):
            if os.path.isfile(p):
                rel = os.path.relpath(p, src).replace("\\", "/")
                if rel not in cfg.get("exclude", []):
                    files.add(rel)

    pending = []
    for rel in sorted(files):
        text = io.open(os.path.join(src, rel), encoding="utf-8", errors="replace").read()
        for value, s, e in scan_groups(text):
            if has_cjk(value) or not value.strip():
                continue
            if value in known:
                continue
            if not re.search(r"[A-Za-z]{2}", value):
                continue
            if not looks_like_ui(value):
                continue
            stmt = text[statement_start(text, s) : s]
            if NON_UI_CALLS.search(stmt):
                continue
            call = UI_CALLS.search(stmt)
            if not call:
                continue
            line = text.count("\n", 0, s) + 1
            pending.append({"file": rel, "line": line, "call": call.group(0), "en": value})

    with io.open(args.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(pending, f, ensure_ascii=False, indent=1)

    if args.markdown:
        with io.open(args.markdown, "w", encoding="utf-8", newline="\n") as f:
            f.write(f"# 待翻译字符串 {len(pending)} 条\n\n")
            cur = None
            for p in pending:
                if p["file"] != cur:
                    cur = p["file"]
                    f.write(f"\n## {cur}\n\n")
                f.write(f"- `{p['en']}`\n  - {p['call']} @ 第 {p['line']} 行\n")

    print(f"待翻译候选 {len(pending)} 条 -> {args.out}")
    for p in pending[:15]:
        print(f"  {p['file']}:{p['line']}  [{p['call']}]  {p['en'][:80]!r}")
    if len(pending) > 15:
        print(f"  ... 另有 {len(pending) - 15} 条")

    # 无新增时退出码 0；有新增退出码 3，供 CI 判断是否需要翻译
    sys.exit(3 if pending else 0)


if __name__ == "__main__":
    main()
