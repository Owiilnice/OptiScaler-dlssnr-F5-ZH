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
    r"Selectable|MenuItem|RadioButton|Combo|BeginCombo|CollapsingHeader|SliderFloat|SliderInt|InputText|InputFloat|"
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

# 下拉/枚举选项数组的声明行。ImGui::Combo(label, &v, names, N) 的第 3 个参数是个
# 字符串数组，而这些数组**不在任何 ImGui 调用里**，光靠调用白名单整类扫不到 ——
# 上游 0.1.26 的「测光/色彩编码/调试视图」，0.1.27 的 Bicubic/Lanczos/MAGIC/
# Native look 全栽在这。这类数组长这样：
#     static const char* dsNames[] = { "FSR1", "Bicubic", "Catmull-Rom", ... };
#     static const char* const meterNames[] = { "Average", "Percentile (log)" };
# 判定：一行里出现 char 指针数组声明，且初始化列表在当前行或后续若干行内闭合。
# 名字后缀 Names/names 也认，但真正的判据是「有 char* 数组初始化」，避免误伤
# 数字数组或设备名单。
ARRAY_DECL = re.compile(
    r"\b(?:static\s+)?(?:const\s+)?char\s*(?:const\s*)?\*\s*(?:const\s*)?"
    r"([A-Za-z_]\w*)\s*\[\s*\]\s*=\s*\{"
)

# 值看起来像「给用户看的选项名」而不是资源路径 / ini 键 / 内部标识。
# 数组里的项通常没有冒号也没有空格（"Bicubic" / "Linear HDR"），所以不能沿用
# looks_like_ui 的「必须有空格或冒号或首字母大写」判据 —— 那个判据正是漏检主因。
NOT_UI_ARRAY_VALUE = re.compile(r"(?:^##|^%|\\\\|/|\.(?:dll|ini|ttf|ttc|json|log|txt|hlsl|cube|exe|bin)$|^[0-9.]+$)")


def ui_array_names(text):
    """找出真正当作界面选项来源的 char* 数组名。

    光看「是不是 char* 数组」不行：`kGroups`（内核分组名）、`states`
    （D3D12 资源状态）、`kDevice`（Vulkan 扩展名）、`models` 都是内部枚举名，
    翻了会破坏功能。判据收紧成：数组名出现在 Combo / BeginCombo / Selectable /
    RadioButton 的参数里 —— 那才是「这些字符串会显示在界面上」的证据。
    """
    names = set()
    for m in re.finditer(
        r"\b(?:ImGui::)?(?:Combo|BeginCombo|Selectable|RadioButton)\s*\(", text
    ):
        # 取该调用的一对括号范围
        depth = 0
        i = text.find("(", m.start())
        j = i
        n = len(text)
        while j < n:
            if text[j] == "(":
                depth += 1
            elif text[j] == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        for ident in re.findall(r"\b([A-Za-z_]\w*)\b", text[i:j]):
            names.add(ident)
    return names


def array_item_ok(value):
    """数组项是否是「给人看的选项名」。"""
    display = value.split("##", 1)[0]
    if not display.strip():
        return False
    if NOT_UI_ARRAY_VALUE.search(display):
        return False
    # 必须含至少两个连续字母，排除纯数字/符号项
    if not re.search(r"[A-Za-z]{2}", display):
        return False
    return True


def find_array_groups(text):
    """切出所有 char* 数组初始化列表里的字面量组。

    返回 [(value, start, end, array_name)]。这些是 ImGui::Combo / RadioButton 的
    选项来源，必须翻译才不会有「界面半中半英」。只有被界面调用引用的数组才算
    （见 ui_array_names），内部枚举名不翻。
    """
    used = ui_array_names(text)
    out = []
    pos = 0
    while True:
        m = ARRAY_DECL.search(text, pos)
        if not m:
            break
        name = m.group(1)
        brace = text.find("{", m.end() - 1)
        if brace < 0:
            pos = m.end()
            continue
        # 找到与 { 配对的 }，越过嵌套的 {} 与字符串
        depth = 0
        i = brace
        n = len(text)
        while i < n:
            c = text[i]
            if c == '"':
                i += 1
                while i < n:
                    if text[i] == "\\":
                        i += 2
                        continue
                    if text[i] == '"':
                        i += 1
                        break
                    i += 1
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
            if c == "/" and i + 1 < n and text[i + 1] == "/":
                j = text.find("\n", i)
                i = n if j < 0 else j + 1
                continue
            if c == "/" and i + 1 < n and text[i + 1] == "*":
                j = text.find("*/", i + 2)
                i = n if j < 0 else j + 2
                continue
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        body = text[brace:i]
        if name in used:
            for value, s, e in scan_groups(body):
                out.append((value, brace + s, brace + e, name))
        pos = i + 1
    return out


def looks_like_ui(value):
    """判断这个字面量像不像「给用户看的文案」。

    先剥掉 ##xxx 控件 ID 后缀，只看 ## 前面的显示部分。
    不剥的话，"Delete##" / "Cancel##tune" / "Apply##tune" 这类单词按钮会被
    「无空格、无冒号、也不是首字母大写的单词」这条判据当成非文案丢掉 ——
    它们恰恰是最典型的按钮文字。上游 v0.1.26 新增的调优/测量面板就栽在这里：
    8 个按钮标签全程没被扫出来，界面上一直是英文。
    """
    display = value.split("##", 1)[0]
    if not display:
        return False          # "##id" 形式：没有显示文字，纯内部 ID
    if NOT_UI_VALUE.search(display):
        return False
    if " " not in display and ":" not in display and not BARE_WORD_OK.match(display):
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
    # untranslated.json 是「已判定保持英文」的清单。不把它算作已知的话，
    # 这些串每轮都会被重新扫出来、重新送翻译、重新被判定保持英文 —— 纯噪声。
    unt_path = os.path.join(DICT, "untranslated.json")
    if os.path.exists(unt_path):
        with io.open(unt_path, encoding="utf-8") as f:
            known |= set(json.load(f))

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

        # 选项数组单独走一路：它们不在任何 ImGui 调用里，UI_CALLS 白名单扫不到。
        for value, s, e, aname in find_array_groups(text):
            if has_cjk(value) or not value.strip():
                continue
            if value in known:
                continue
            if not array_item_ok(value):
                continue
            line = text.count("\n", 0, s) + 1
            pending.append({"file": rel, "line": line, "call": f"array:{aname}", "en": value})

    pending.sort(key=lambda p: (p["file"], p["line"]))

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
