#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""一次性：把上游 c62aae9 新增/改写的界面文案补进词典。

用法（在同步过上游源码的工作区里跑）：
    python tools/apply_new_strings.py <源码根目录>

做的事：
  1. 删掉 4 条上游已彻底删除的词典条目（避免「未命中」噪声盖住真问题）
  2. 用上游改写后的新文案替换 5 条旧键，并给出新译文
  3. 新增 42 条译文
  4. 把 12 条诊断/标识串写进 untranslated.json，注明「保持英文」的理由
"""
import io, json, os, sys, glob, re

SRC = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
DICT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "dict")
DICT = os.path.normpath(DICT)


def load(name):
    p = os.path.join(DICT, name)
    with io.open(p, encoding="utf-8") as f:
        return json.load(f)


def save(name, obj):
    p = os.path.join(DICT, name)
    with io.open(p, "w", encoding="utf-8", newline="\n") as f:
        json.dump(obj, f, ensure_ascii=False, indent=1)


# ---------------------------------------------------------------- 1. 上游已删
DEAD = [
    "Allow approximate FG camera guides (experimental)",
    "NR every second frame (NVIDIA Frame Generation, experimental)",
    "Re-calibrate##autoexposure",
    "Run NR every other rendered frame and use NVIDIA Frame Generation (FG) to interpolate its changes.\\nRequires the option above. Adds one rendered frame of latency and may misalign effects or UI.\\nIf motion vectors are unavailable, each NR result is reused for two frames.",
    "Use estimated camera data when the game does not provide it. May cause artifacts during camera movement.",
]

# ------------------------------------------------- 2. 文案被上游改写（旧键 -> 新键）
# 键是旧英文原文的前缀，值是新英文原文的前缀；实际新键从源码里现取，避免手抄出错。
REKEY = {
    "NVIDIA: original FP8 model": "NVIDIA: original FP8 model",
    "Choose how HDR brightness is mapped": "Choose how HDR brightness is mapped",
    "For games that hand over": "For games that hand over",
    "Learns the calibration against": "Learns the calibration against",
    "{:.5g} (white point it gives": "{:.5g} (white point it gives",
}

# 新译文，键为「新英文原文的前缀」
NEW_ZH_BY_PREFIX = {
    "NVIDIA: original FP8 model":
        "NVIDIA：原始 FP8 模型（默认），部分敏感运算保持更高精度。\\n"
        "实验性：此分支面向 RTX 50 显卡的 FP8+NVFP4 混合方案；输出可能略有差异。仅支持 D3D12。",

    "Choose how HDR brightness is mapped":
        "选择 NR 的 HDR 亮度映射方式。\\n"
        "软拐点会压缩高光。可逆曲线使用可逆映射。均衡模式保留中间调并压缩高光。\\n"
        "HLG 和 PQ 是广播级 HDR 曲线：白点分别位于 75%（HLG）和 58%（PQ），给高光留出更多余量"
        "（HLG 约为白点的 4 倍，PQ 约 50 倍），但模型看到的中间调会有所不同。"
        "若用过「调优」，更换曲线后请重新运行一次。\\n"
        "合成模式使用强度控制和下方的「高光保护」（仅限制提亮；合成模式下变暗不受限）。"
        "替换模式绕过强度控制（模型结果直接应用，不做合成），但同一个「高光保护」数值仍会在两个方向上限制它"
        "——若替换模式在明亮高光附近闪烁或出现色带，请调低该值。",

    "For games that hand over":
        "适用于在应用自身曝光之前就把画面交出来的游戏：已知会这样做的游戏（如 RDR2）默认开启，\\n"
        "其余游戏默认关闭。自动模式会在游玩最初几秒内学习自身测光与游戏曝光的关系，\\n"
        "随后跟随游戏的曝光，因此亮度会与游戏完全同步：过场、菜单、淡入淡出。"
        "在自身曝光从不变化的游戏里，若两者短时间内相差超过 0.75 EV，\\n"
        "校准会向自动模式靠拢（每秒最多 0.25 EV）。\\n"
        "亮度滑块保持原有含义。对于自行处理曝光的游戏（大多数游戏）请保持关闭：\\n"
        "否则会把它们的曝光叠加两次。在 Vulkan 上它会比游戏滞后几帧。",

    "Learns the calibration against":
        "重新学习与游戏曝光之间的校准关系，例如当校准是在过场动画或加载画面中\\n"
        "学到的时候。此期间会暂时使用普通的自动模式（约 2 秒）。\\n"
        "请在普通白天场景中重新学习，而不是雪地、夜晚或室内。之后每当两者\\n"
        "相差超过 0.75 EV 时，校准会自动向自动模式靠拢。",

    "{:.5g} (white point it gives":
        "{:.5g}（它给出的白点：{:.4g}）{}{}",
}

# 这条是纯字符串替换（没有文案改写，只是控件名换了）
SIMPLE_REKEY = {"Re-calibrate##autoexposure": "Re-learn##autoexposure"}

# ------------------------------------------------------------- 3. 新增译文
# 键 = 英文原文前缀（唯一即可），值 = 中文。完整英文从源码里现取。
NEW = [
    ("Best here: %+.1f EV (now", "此处最佳：%+.1f EV（当前 %+.1f EV）"),
    ("Best here: %+.1f EV, as it is now.", "此处最佳：%+.1f EV，与当前一致。"),
    ("Tune again##tune", "重新调优##tune"),
    ("Score from %+.1f EV (left)", "评分从 %+.1f EV（左）到 %+.1f EV（右）。最佳：带通 %+.1f，原始 %+.1f。"),
    ("Apply raw instead##tune", "改用原始值##tune"),
    ("Tune for this scene", "针对当前场景调优"),
    ("Finds the Model input brightness above",
     "寻找能让 NR 在当前屏幕画面中获得最多细节的模型输入亮度。\\n"
     "它会在滑块的可用范围内逐点尝试，每步约 12 帧，并检查每一步的\\n"
     "细节、闪烁和裁切情况。运行期间请保持镜头不动：画面会故意忽明忽暗。\\n"
     "在你按下「应用」之前不会有任何改动。结果是一个曝光偏移量，\\n"
     "因此之后仍会跟随场景变化。若有多个模型遍次，它只运行并\\n"
     "测量第一遍：第一遍才是看到游戏画面的那一次，所以结果对\\n"
     "任意遍数都成立。若开启「跟随游戏的曝光」，它会针对自动模式\\n"
     "自身的曝光进行调优，并在运行期间重新学习跟随关系，因此跟随接管后结果依然成立。\\n"
     "若 NR 在超分辨率之前运行，该过程本身在 SR 之后执行（画面会短暂变化），\\n"
     "结束后 NR 会回到 SR 之前；该设置不会被改变。"),
    ("Not available: %s", "不可用：%s"),
    ("Could not start: %s", "无法启动：%s"),
    ("Stopped: %s", "已停止：%s"),
    ("Measure detail", "测量细节"),
    ("Measures NR's output on the scene",
     "在当前设置下测量 NR 对屏幕画面的输出，约需一秒：它相对游戏帧\\n"
     "增加了多少细节，以及它超出游戏自身帧间变化之外的闪烁程度。\\n"
     "请在静止画面（暂停的回放、拍照模式）上按下它，改一项设置，再按一次：\\n"
     "与上一次测量相比的变化就反映了该设置的作用。连续跑两次可以看出噪声水平。\\n"
     "除此之外不会有任何改动；数值也会写入 OptiScaler.log。"),
    ("#%u  Detail added", "#%u  细节增加 %.5f  闪烁 %.5f"),
    ("detail out %.5f in %.5f, raw",
     "细节 出 %.5f 入 %.5f，原始 %.5f | 变化 出 %.5f 入 %.5f | %u 帧"),
    ("vs #%u: not comparable",
     "对比 #%u：不可比较（测量时的曝光不同：请等画面稳定，或白点来源已改变）"),
    ("vs #%u: detail %+.1f%%",
     "对比 #%u：细节 %+.1f%%，闪烁 %+.1f%%，原始 %+.1f%%"),
    ("Running - %.2f ms elapsed",
     "运行中 —— 每帧平均耗时 %.2f ms（%.2f 至 %.2f）%s"),
    ("Easing the calibration toward",
     "正在把校准向自动模式自身的曝光靠拢（相差 %+.1f EV）。"),
    ("Eye adaptation", "眼睛适应"),
    ("How quickly Automatic follows a change",
     "自动模式跟随场景亮度变化的速度，类似眼睛的适应过程。\\n"
     "可避免镜头拉近、或短暂扫过暗色人群、明亮地面时\\n"
     "画面亮度与色调出现起伏。游戏明确告知的切换会立即跟随。\\n"
     "经过这段时间后会跟随约三分之二的变化量。关闭则每帧立即跟随。\\n"
     "跟随游戏曝光时不使用此项：由游戏自身的适应逻辑处理。"),
    ("Reuse bottleneck: off in Vulkan",
     "复用瓶颈：Vulkan 游戏中关闭（复用结果可能在暗场景中闪一下）"),
    ("Bottleneck reuse: off while",
     "瓶颈复用：在「帧间复用细节」运行期间关闭"),
    ("Reuse detail between frames (experimental)",
     "帧间复用细节（实验性）"),
    ("Runs the model every other frame",
     "每隔一帧运行模型。中间那些帧则用运动矢量把上一次结果的细节搬到新帧上，\\n"
     "在深度或颜色不一致的地方丢弃该细节。\\n"
     "任意遍数下大致都能把 NR 的 GPU 开销减半。物体移动并露出新区域时细节可能突然跳出。\\n"
     "仅支持 NR 在 SR 之后的 D3D12 和 Vulkan。运行期间「复用瓶颈」会关闭。\\n"
     "帧生成开启时会自动关闭（除非在「调试 > 帧生成时保持开启」中强制）："
     "生成帧是由真实帧插出来的，\\n完整帧与复用帧交替会让它出现闪烁。"),
    ("Show dropped detail", "显示被丢弃的细节"),
    ("On reused frames, paints magenta",
     "在复用帧上，把被丢弃的搬移细节涂成洋红，把「填充」补上的部分涂成青色。\\n"
     "仅用于测试。"),
    ("Fill dropped detail", "填充被丢弃的细节"),
    ("Where the moved detail had to be dropped",
     "在不得不丢弃搬移细节的地方（例如身体移开露出背景），改用同一表面上\\n"
     "邻近像素的 NR 细节来填充，而不是让该处显示没有 NR 的画面。\\n"
     "在多遍次时尤为重要。开启「显示被丢弃的细节」时，填充区域显示为青色。默认 1。"),
    ("Steady full frames", "稳定完整帧"),
    ("Pulls the model's new detail on full frames",
     "在可信的区域内，把完整帧上模型新增的细节向上一帧搬移过来的细节靠拢，\\n"
     "使完整帧与复用帧差异更小、细节起伏更少。\\n"
     "会给运动中的细节带来一点延迟。0 = 关闭（默认）。"),
    ("Keep on with frame generation", "帧生成时保持开启"),
    ("Keeps reusing detail while frame generation",
     "在帧生成开启时仍持续复用细节，便于与关闭时对比。\\n"
     "可能闪烁：生成帧是由成对的完整帧和复用帧插出来的。"),
    ("Minimum frame rate", "最低帧率"),
    ("Reuse runs only while the rendered frame rate",
     "仅当渲染帧率（帧生成之前）不低于该值时才启用复用；低于该值时每帧都运行模型。\\n"
     "帧率低时帧间位移更大，搬移的细节会在运动物体周围拖尾。\\n"
     "回升到高于最低值 15% 才重新启用。0 = 不设下限。默认 25。"),
    ("Reuse detail: %s (rendered", "复用细节：%s（渲染 %.0f fps）"),
    ("Full NR: %llu", "完整 NR：%llu   复用：%llu   回退：%llu   渲染：%.0f fps"),
    ("NR GPU time per frame", "NR 每帧 GPU 耗时：平均 %.2f ms，%.2f 至 %.2f ms"),
    ("Full and reused frames cost differently",
     "完整帧与复用帧的开销不同，因此游戏的帧时间会交替变化。若运动出现顿挫，\\n"
     "把帧率限制设在略低于平均帧率的位置即可拉平。"),
    ("Hybrid: D3D12 only", "混合：仅 D3D12（Vulkan 上不应用）"),
    ("How NR reads the game's colour",
     "NR 如何读取游戏的颜色。\\n"
     "自动模式信任游戏：采用其 DLSS HDR 标志和缓冲区格式（在「最终图像」上则采用屏幕色彩空间）。\\n"
     "仅当游戏上报颜色有误、导致画面发灰、过暗或出现色带时，才改选其他项。\\n"
     "色调映射的 gamma 2.2 和 PQ 会先转换给模型、再转换回来。"),
    ("Menu", "菜单"),
]

# ------------------------------------------- 4. 保持英文（进 untranslated.json）
KEEP_EN = {
    "FPS: %6.1f/%5.1f, Avg: %6.1f": "FPS 叠加层的诊断读数，参考汉化版保持英文",
    "FGId: %llu, RfxId: %llu": "帧生成内部句柄 ID，参考汉化版保持英文",
    "nvngx_dlss : %s": "DLL 名 + 版本号，参考汉化版保持英文",
    "nvngx_dlssd : %s": "DLL 名 + 版本号，参考汉化版保持英文",
    "nvngx.dll: %s": "DLL 名 + 版本号，参考汉化版保持英文",
    "libxess: %s": "DLL 名 + 版本号，参考汉化版保持英文",
    "FSR 3.1: %s": "DLL 名 + 版本号，参考汉化版保持英文",
    "FSR 3.1 SR: %s": "DLL 名 + 版本号，参考汉化版保持英文",
    "FSR 3.1 FG: %s": "DLL 名 + 版本号，参考汉化版保持英文",
    "Vulkan %s| %s %d.%d.%d%s": "Vulkan 驱动版本行，参考汉化版保持英文",
    "FSR %s": "FSR 版本标签（%s 是版本号），参考汉化版保持英文",
    "FSR %s##%d": "FSR 版本标签（%s 是版本号），参考汉化版保持英文",
    "Splash": "ImGui 窗口名，仅作内部 ID，界面不显示（该窗口无边框无标题）",
}


def scan_groups(text):
    """把 C 相邻字面量合成一个逻辑串，跳过注释和字符字面量。"""
    out = []
    i, n = 0, len(text)
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
            while i < n and text[i] != "'":
                i += 2 if text[i] == "\\" else 1
            i += 1
            continue
        if c == '"':
            start = i
            buf = []
            while i < n and text[i] == '"':
                i += 1
                while i < n and text[i] != '"':
                    if text[i] == "\\":
                        buf.append(text[i:i + 2])
                        i += 2
                    else:
                        buf.append(text[i])
                        i += 1
                i += 1
                while i < n and text[i] in " \t\r\n":
                    i += 1
            out.append(("".join(buf), start, i))
            continue
        i += 1
    return out


def main():
    cfg = load("targets.json")
    groups = set()
    for g in cfg["globs"]:
        for p in glob.glob(os.path.join(SRC, g), recursive=True):
            if not os.path.isfile(p):
                continue
            rel = os.path.relpath(p, SRC).replace("\\", "/")
            if rel in cfg.get("exclude", []):
                continue
            with io.open(p, encoding="utf-8", errors="replace") as f:
                for v, _, _ in scan_groups(f.read()):
                    groups.add(v)

    gdict = load("strings.json")
    before = len(gdict)

    def find(prefix):
        # 先看有没有完全相同的串：Menu / Eye adaptation 这类短串必须精确匹配，
        # 否则会被 "Menu Scale" 之类的长串顶掉
        if prefix in groups:
            return prefix
        hits = [g for g in groups if g.startswith(prefix)]
        if not hits:
            raise SystemExit(f"源码里找不到以 {prefix!r} 开头的串")
        if len(hits) > 1:
            # 取最长的（前缀相同时更完整的那条）
            hits.sort(key=len, reverse=True)
            print(f"  注意: {prefix!r} 匹配 {len(hits)} 条，取最长的")
        return hits[0]

    # 1. 删上游已删
    for k in DEAD:
        if k in gdict:
            del gdict[k]
            print(f"删除（上游已删）: {k[:60]!r}")

    # 2. 改写文案换键
    for old_pref, new_pref in REKEY.items():
        old = [k for k in gdict if k.startswith(old_pref)]
        if not old:
            print(f"  跳过（词典里没有）: {old_pref!r}")
            continue
        assert len(old) == 1, old
        old = old[0]
        new = find(new_pref)
        if old == new:
            print(f"  文案未变，只更新译文: {new_pref!r}")
        del gdict[old]
        gdict[new] = NEW_ZH_BY_PREFIX[new_pref]
        print(f"换键: {old[:50]!r} -> {new[:50]!r}")

    # 2b. 纯控件名替换
    for old, new in SIMPLE_REKEY.items():
        if old in gdict:
            zh = gdict.pop(old)
            gdict[new] = zh.replace("重新校准", "重新学习")
            print(f"换键: {old!r} -> {new!r}")

    # 3. 新增
    added = 0
    for pref, zh in NEW:
        en = find(pref)
        if en in gdict:
            if gdict[en] != zh:
                print(f"  已存在且译文不同，覆盖: {en[:50]!r}")
                gdict[en] = zh
            continue
        gdict[en] = zh
        added += 1
    print(f"\n新增 {added} 条")

    # 4. 保持英文
    parked = load("untranslated.json") if os.path.exists(
        os.path.join(DICT, "untranslated.json")) else {}
    for en, why in KEEP_EN.items():
        parked[en] = {"why": why, "seen_in": ["OptiScaler/menu/menu_common.cpp"]}

    save("strings.json", {k: gdict[k] for k in sorted(gdict)})
    save("untranslated.json", {k: parked[k] for k in sorted(parked)})
    print(f"词典 {before} -> {len(gdict)} 条")
    print(f"待裁决 {len(parked)} 条")


if __name__ == "__main__":
    main()
