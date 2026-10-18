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
#
# 2026-10-18 补了一批「提示类」调用（此前整类漏掉，实测 19 条全英文）：
#   SeparatorText / SeparatorTextEx  分区标题（Low latency、Look Presets、NR Exposure…）
#   SetTooltip / SetItemTooltip      悬停提示正文
#   BeginItemTooltip                 同上（ImGui 1.90+ 的写法）
#   ProgressBar                     进度条上的文字（"Starting..." / "Measuring: hold the camera still"）
#   InsertNotification              右下角通知弹窗
#   TableSetupColumn                表头文字
#   InputInt / InputScalar          输入框标签
#   PlotLines / PlotHistogram       图表标签
#   ColorEdit3 / ColorEdit4         取色器标签
#   CheckboxFlags                   复选标签
#   TextLinkOpenURL                 可点链接文字
UI_CALLS = re.compile(
    r"\b(?:ImGui::(?:Text|TextWrapped|TextDisabled|TextColored|TextUnformatted|Button|SmallButton|Checkbox|"
    r"Selectable|MenuItem|RadioButton|Combo|BeginCombo|CollapsingHeader|SliderFloat|SliderInt|InputText|InputFloat|"
    r"BeginTabItem|Begin|BeginChild|PushID|TreeNode|TreeNodeEx|LabelText|BulletText|"
    r"SeparatorText|SeparatorTextEx|SetTooltip|SetItemTooltip|BeginItemTooltip|ProgressBar|InsertNotification|"
    r"TableSetupColumn|InputInt|InputScalar|PlotLines|PlotHistogram|ColorEdit3|ColorEdit4|CheckboxFlags|"
    r"TextLinkOpenURL)"
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
# 允许一个连字符复合词（"Band-pass" / "Anti-Lag"），否则图表标签 "Band-pass##tune"
# 会被当成技术标识丢掉。
BARE_WORD_OK = re.compile(r"^[A-Z][a-z]{2,}(?:-[A-Za-z]+)*$")

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


def strip_comments(text):
    """把注释内容替换成等长空格，返回同长度字符串（偏移不变）。

    结构性分析（找函数定义、找调用参数）必须在去注释的文本上做 —— 注释里的
    括号和花括号会让正则匹配出根本不存在的「函数名」。实测：一段注释里的
    `only (upscalers/...)` 让 `only` 变成「UI helper」，进而把一个 ini 配置值
    数组 `stored[]` 误报成界面文案。
    """
    out = list(text)
    i = 0
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
            j = n if j < 0 else j
            out[i:j] = " " * (j - i)
            i = j
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out[i:j] = " " * (j - i)
            i = j
            continue
        i += 1
    return "".join(out)


def call_arg_identifiers(text, open_idx):
    """取 text[open_idx]（'('）这一对括号内出现的所有标识符。"""
    close = match_paren(text, open_idx)
    return set(re.findall(r"\b([A-Za-z_]\w*)\b", text[open_idx:close]))


def match_paren(text, open_idx):
    """返回与 text[open_idx]（'('）配对的 ')' 的下标；找不到返回 len(text)。"""
    depth = 0
    i = open_idx
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
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return n


# 会出现在 if / for 这类控制语句前的关键字，别当成函数名。
CTRL_KEYWORDS = {"if", "for", "while", "switch", "catch", "return", "do", "else", "sizeof"}

# 本地函数的定义（`Name(args) {` 或 `Name(args) const {`）。
FUNC_DEF = re.compile(r"\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:const\s*)?\{")


def ui_helper_names(text):
    """函数体里含 ImGui 界面调用的本地函数名。

    这些函数的参数会被转发到界面上，所以调用方传进去的数组同样是界面文案来源。
    例：`InheritedProfileCombo(label, opt, names, nameCount)` 内部调
    `ImGui::Combo(label, &selected, names, nameCount)`，调用方传的
    `styles` / `presets` 数组不在任何 Combo 调用里 —— 只看 ImGui 调用会整类漏掉。
    """
    out = set()
    for m in FUNC_DEF.finditer(text):
        name = m.group(1)
        if name in CTRL_KEYWORDS:
            continue
        open_idx = text.find("{", m.end() - 1)
        if open_idx < 0:
            continue
        if UI_CALLS.search(text[open_idx : match_brace(text, open_idx)]):
            out.add(name)
    return out


def ui_array_names(text):
    """找出真正当作界面选项来源的 char* 数组名。

    光看「是不是 char* 数组」不行：`kGroups`（内核分组名）、`kDevice`
    （Vulkan 扩展名）、`models` 都是内部枚举名，翻了会破坏功能。判据收紧成：
    数组名出现在「会把参数画到界面上」的调用里 —— 直接调 ImGui 的
    Combo / BeginCombo / Selectable / RadioButton，或调一个内部含 ImGui
    调用的本地 helper（见 ui_helper_names）。
    """
    callees = {"Combo", "BeginCombo", "Selectable", "RadioButton"} | ui_helper_names(text)
    names = set()
    for m in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", text):
        if m.group(1) not in callees:
            continue
        open_idx = text.find("(", m.end() - 1)
        names |= call_arg_identifiers(text, open_idx)
    return names


def match_brace(text, open_idx):
    """返回与 text[open_idx]（'{'）配对的 '}' 的下标；找不到返回 len(text)。

    跳过字符串、字符字面量与注释，所以初始化列表里的 `"}"` 不会误判。
    """
    depth = 0
    i = open_idx
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
                return i
        i += 1
    return n


# 结构体/类定义，用来判断某个数组的元素类型是不是「装界面文案的结构体」。
STRUCT_DECL = re.compile(r"\b(?:struct|class)\s+([A-Za-z_]\w*)[^;{}]*\{")

# 聚合数组声明：`inline constexpr PageInfo kPages[] = { { "id", "Label" }, ... };`
# 元素类型不是 char*，而是自定义结构体。上游 0.1.27 的主菜单左导航就是这样：
#     struct PageInfo { const char* name; const char* label; };
#     inline constexpr PageInfo kPages[] = { { "upscaler", "Upscaler" }, ... };
# 这一整类 ARRAY_DECL 匹配不到（那个正则要求 `char *`），且数组也不出现在任何
# ImGui 调用里，ui_array_names 也捞不着 —— 于是导航栏整排标签漏译、且零告警。
AGG_ARRAY_DECL = re.compile(
    r"\b(?:inline\s+)?(?:constexpr\s+)?(?:static\s+)?([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*\[\s*\]\s*=\s*\{"
)


def string_struct_types(text):
    """找出「成员里有 const char* 字段」的结构体名。

    这类结构体的数组元素几乎一定是界面文案（name/label 成对）。判据是结构体定义体
    里出现 `const char*` —— 数字结构体（float 数组、D3D12_RESOURCE_BARRIER 之类）
    不含，因此不会被误伤。
    """
    out = set()
    for m in STRUCT_DECL.finditer(text):
        open_idx = text.find("{", m.end() - 1)
        if open_idx < 0:
            continue
        close = match_brace(text, open_idx)
        if re.search(r"\bconst\s+char\s*\*", text[open_idx:close]):
            out.add(m.group(1))
    return out


def array_item_ok(value):
    """数组项是否是「给人看的选项名」。

    只靠「元素类型里有 const char*」还不够 —— `HybridAssets::Entry{path,...}`
    （文件路径）、`KernelProfile::Rule{part,...}`（内核名片段）也是 const char*，
    但都是内部标识。加两条判据把它们挡掉：
      1. 纯小写 = ini 键名 / 路径 / 内核名（kPages 的 "upscaler" "nr.status" 就是
         ini 键，绝不能翻）。界面标签至少含一个大写字母 —— Upscaler / NR only /
         Off / FG only (game's upscaler) 都满足。
      2. 值里带分隔符（`_` `.` `/` `\\`）= 标识符或路径。
    """
    display = value.split("##", 1)[0]
    if not display.strip():
        return False
    if NOT_UI_ARRAY_VALUE.search(display):
        return False
    if re.search(r"[_./\\]", display):
        return False
    if not re.search(r"[A-Z]", display):
        return False
    # 必须含至少两个连续字母，排除纯数字/符号项
    if not re.search(r"[A-Za-z]{2}", display):
        return False
    return True


def _collect_array(text, brace, name, out):
    body = text[brace : match_brace(text, brace)]
    for value, s, e in scan_groups(body):
        out.append((value, brace + s, brace + e, name))


def find_array_groups(text):
    """切出所有「界面文案数组」初始化列表里的字面量组。

    返回 [(value, start, end, array_name)]。两类：
      1. `char* xxx[] = {...}` —— ImGui::Combo / RadioButton 的选项来源。只有被
         界面调用引用的才算（见 ui_array_names），内部枚举名（kGroups / kDevice
         / states 里的非界面用途）不翻。
      2. 聚合数组 `Type xxx[] = { {...} }`，且 Type 的成员里有 const char* ——
         如主菜单左导航的 PageInfo kPages[]。这类不需要 ui_array_names 佐证：
         「元素类型装的是字符串」本身就是「这是文案」的证据。
    """
    code = strip_comments(text)
    used = ui_array_names(code)
    struct_types = string_struct_types(code)
    out = []
    pos = 0
    while True:
        m = ARRAY_DECL.search(code, pos)
        if not m:
            break
        brace = code.find("{", m.end() - 1)
        if brace < 0:
            pos = m.end()
            continue
        if m.group(1) in used:
            _collect_array(text, brace, m.group(1), out)
        pos = match_brace(code, brace) + 1

    pos = 0
    while True:
        m = AGG_ARRAY_DECL.search(code, pos)
        if not m:
            break
        brace = code.find("{", m.end() - 1)
        if brace < 0:
            pos = m.end()
            continue
        if m.group(1) in struct_types:
            _collect_array(text, brace, m.group(2), out)
        pos = match_brace(code, brace) + 1
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
    """回退到所在语句的开头，用来判断这个字面量属于哪个调用。

    `{` 不一定是语句边界：`ImGui::InsertNotification({ type, ms, "文本" })` 里的
    `{` 是聚合初始化列表。紧跟在 `(` `,` `=` `{` 后面的 `{` 要跳过继续往前找，
    否则语句头会落在 `{` 之后，调用名被切掉、整条漏检（实测漏了右下角那条
    「No upscaler call from this game so far」通知）。
    """
    i = pos
    while True:
        s = max(text.rfind(";", 0, i), text.rfind("{", 0, i), text.rfind("}", 0, i))
        if s < 0:
            return 0
        if text[s] == "{":
            j = s - 1
            while j >= 0 and text[j] in " \t\r\n":
                j -= 1
            if j >= 0 and text[j] in "(,={":
                i = s          # 初始化列表的 {，不是语句边界
                continue
        return s + 1


# 变量形式的界面文本：先存进变量、再把变量名传给界面调用。
#   static const char* presetHelp = "Which model NR runs on this pass. ...";
#   HelpMarker(presetHelp);
# 这类字面量不在任何调用里（调用拿到的是变量名），UI_CALLS 白名单和数组扫描都
# 捞不到 —— 实测 0.1.32 的 presetHelp（外观预设的说明）就栽在这。
VAR_DECL = re.compile(
    r"\b(?:static\s+)?(?:constexpr\s+)?(?:const\s+)?char\s*(?:const\s*)?\*\s*(?:const\s*)?"
    r"([A-Za-z_]\w*)\s*=\s*"
)


def _stmt_end(text, i):
    """从 i 往后找第一个「字符串外」的分号，返回其下标（找不到返回 len）。

    不能直接 `text.find(";", i)`：提示文本自己会带分号
    （`"... the small model (about half the cost, softer; only in 310.9 DLLs)"`），
    直接找会把初始化表达式截断在半句话上。
    """
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
        if c == ";":
            return i
        i += 1
    return n


def find_var_text_groups(text):
    """找「存进变量再传给界面」的提示文本。

    三条判据全满足才收（宁可漏，不可错 —— 翻错 ini 键会直接破坏功能）：
      1. 声明是 char* 类型（含 const / static / constexpr 组合）；
      2. 初始化表达式里有字符串字面量；
      3. **同文件**里该变量名出现在界面调用的参数里。

    跨文件的状态文本（`DetailReuseInfo.why`、`ExposureScan::Status()` 这类
    经过结构体字段或返回值转手的）不在此列 —— 静态追踪成本太高，
    靠技能文档里的「变量/返回值形式的提示」清单人工兜。
    """
    code = strip_comments(text)
    ui_spans = []
    for m in UI_CALLS.finditer(code):
        oi = code.find("(", m.end() - 1)
        if oi < 0:
            continue
        ui_spans.append((oi, match_paren(code, oi)))

    out = []
    for m in VAR_DECL.finditer(code):
        name = m.group(1)
        semi = _stmt_end(code, m.end())
        if semi >= len(code):
            continue
        expr = text[m.end() : semi]
        if '"' not in expr:
            continue
        pat = re.compile(r"\b" + re.escape(name) + r"\b")
        if not any(pat.search(code[a:b]) for a, b in ui_spans):
            continue
        for value, s, e in scan_groups(expr):
            out.append((value, m.end() + s, m.end() + e, name))
    return out


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
    # 词典的「值」也是已知串。译文里有一部分刻意保留英文（F5Low / Native look /
    # Lanczos2 / DLAA 这类专有名），它们不含汉字，has_cjk 拦不住 —— 不把值算作
    # 已知的话，重放之后源码里的 "F5Low" 会被当成新串反复报出来，CI 还会因此
    # 走进「自动翻译」分支。一个串等于某条译文，就说明它已经处理过了。
    known |= {v for v in gdict.values() if v}

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

        # 变量形式的提示文本（存进变量再传给界面调用）单独一路。
        for value, s, e, vname in find_var_text_groups(text):
            if has_cjk(value) or not value.strip():
                continue
            if value in known:
                continue
            if not re.search(r"[A-Za-z]{2}", value):
                continue
            if not looks_like_ui(value):
                continue
            line = text.count("\n", 0, s) + 1
            pending.append({"file": rel, "line": line, "call": f"var:{vname}", "en": value})

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
