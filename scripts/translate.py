#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把 scan_strings.py 报出的新串交给大模型翻译，回写进 dict/strings.json。

用法:
    set DEEPSEEK_API_KEY=sk-xxx
    python scripts/translate.py --input pending.json

两条出路，模型必须二选一：
    翻译  -> 写进 dict/strings.json，下次重放自动生效
    SKIP  -> 写进 dict/untranslated.json 并附理由，等人裁决

SKIP 这条路是刻意留的。上游界面串里有相当一部分「看着像文案、其实是功能串」：
DLL 名、ini 键名、ImGui 控件 ID、格式串占位的诊断读数。翻错了会出 bug，
所以宁可让模型显式说「这条我不翻」，也不要它硬译。

写回后需要人工过一眼 git diff 再提交 —— CI 里是自动提交的，但每次提交都在
仓库历史里可追溯、可回滚。
"""
import argparse
import io
import json
import os
import re
import sys
import urllib.error
import urllib.request

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DICT = os.path.join(ROOT, "dict")

API_URL = "https://api.deepseek.com/chat/completions"
MODEL = "deepseek-chat"
BATCH = 30

SYSTEM = """你是 OptiScaler（一个游戏画面放大/帧生成工具）的简体中文本地化译者。
OptiScaler 是一个注入到游戏里的 ImGui 覆盖菜单，用户是普通玩家。

你的任务：把界面英文字符串翻成简体中文，或判定它不该翻。

必须判为 SKIP 的情况（一条都不许翻）：
1. DLL / 文件名 / 路径：nvngx.dll、libxess.dll、OptiScaler.ini、D3D12_OptiScaler
2. ini 配置键名、枚举名、内部标识符：fg_enabled、kNames、MaxPerf
3. ImGui 控件 ID：以 ## 开头的串，或者整串就是窗口名（如 "Splash"）
4. 纯格式串 / 诊断读数：只由占位符和缩写组成的串，如 "FSR %s"、"FGId: %llu, RfxId: %llu"、
   "nvngx_dlss : %s"、"Vulkan %s| %s %d.%d.%d%s"。判据：去掉占位符和全大写缩写后，
   剩下的部分凑不出一个正常的英文短语，那它就是读数而不是文案。
5. 日志、断言、异常消息（这些不会出现在界面上）
6. 已经是中文的串

翻译时的硬性要求：
- 占位符 %s %d %u %.2f %llu %% {0} 原样保留，数量和顺序不变
- \\n 转义原样保留在同样位置
- ## 后缀原样保留（如 "Reset##" -> "重置##"）
- 术语严格按下面给的术语表
- 中文用全角标点，但紧邻代码/数字处用半角

只输出 JSON，不要任何解释：
{"translations": {"<英文原文>": "<中文>"}, "skipped": {"<英文原文>": "<一句话理由>"}}
每个输入串必须恰好出现在 translations 或 skipped 之一里。"""


def post(payload, key):
    req = urllib.request.Request(
        API_URL,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json", "Authorization": f"Bearer {key}"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=180) as r:
        return json.loads(r.read().decode("utf-8"))


def relevant_pairs(gdict, batch, limit=80):
    """挑出和本批次共享词的既有译法，作为少样本示例喂给模型，保证说法一致。"""
    words = set()
    for s in batch:
        words |= {w.lower() for w in re.findall(r"[A-Za-z]{4,}", s)}
    scored = []
    for en, zh in gdict.items():
        overlap = len(words & {w.lower() for w in re.findall(r"[A-Za-z]{4,}", en)})
        if overlap:
            scored.append((overlap, en, zh))
    scored.sort(reverse=True)
    return {en: zh for _, en, zh in scored[:limit]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    key = os.environ.get("DEEPSEEK_API_KEY", "").strip()
    if not key:
        print("缺少 DEEPSEEK_API_KEY，跳过自动翻译")
        sys.exit(0)

    pending = json.load(io.open(args.input, encoding="utf-8"))
    if not pending:
        print("无待翻译字符串")
        sys.exit(0)

    strings = []
    for p in pending:
        if p["en"] not in strings:
            strings.append(p["en"])

    gdict = json.load(io.open(os.path.join(DICT, "strings.json"), encoding="utf-8"))
    glossary = json.load(io.open(os.path.join(DICT, "glossary.json"), encoding="utf-8"))

    acc_path = os.path.join(DICT, "untranslated.json")
    parked = json.load(io.open(acc_path, encoding="utf-8")) if os.path.exists(acc_path) else {}

    done = {}
    for i in range(0, len(strings), BATCH):
        batch = strings[i : i + BATCH]
        user = {
            "术语表": glossary["terms"],
            "文风要求": glossary["style"],
            "既有译法参考": relevant_pairs(gdict, batch),
            "待翻译": batch,
        }
        print(f"[{i // BATCH + 1}/{(len(strings) + BATCH - 1) // BATCH}] {len(batch)} 条")
        try:
            resp = post(
                {
                    "model": MODEL,
                    "messages": [
                        {"role": "system", "content": SYSTEM},
                        {"role": "user", "content": json.dumps(user, ensure_ascii=False)},
                    ],
                    "response_format": {"type": "json_object"},
                    "temperature": 0.2,
                },
                key,
            )
            data = json.loads(resp["choices"][0]["message"]["content"])
        except (urllib.error.URLError, KeyError, ValueError) as e:
            print(f"  调用失败，本批跳过: {e}")
            continue

        for en, zh in (data.get("translations") or {}).items():
            if en in batch and isinstance(zh, str) and zh.strip() and zh != en:
                done[en] = zh
        for en, why in (data.get("skipped") or {}).items():
            if en in batch:
                parked[en] = {"why": str(why)[:200], "seen_in": sorted(
                    {p["file"] for p in pending if p["en"] == en})}

    # 兜底：模型漏答的串不猜，直接进待裁决
    for en in strings:
        if en not in done and en not in parked:
            parked[en] = {"why": "模型未给出判定，留待人工", "seen_in": sorted(
                {p["file"] for p in pending if p["en"] == en})}

    print(f"\n译出 {len(done)} 条，待裁决 {len(parked)} 条")
    if args.dry_run:
        print(json.dumps(done, ensure_ascii=False, indent=1)[:3000])
        return

    gdict.update(done)
    with io.open(os.path.join(DICT, "strings.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump({k: gdict[k] for k in sorted(gdict)}, f, ensure_ascii=False, indent=1)
    with io.open(acc_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump({k: parked[k] for k in sorted(parked)}, f, ensure_ascii=False, indent=1)
    print(f"词典现有 {len(gdict)} 条 -> dict/strings.json")
    print("待裁决清单 -> dict/untranslated.json（人工过一眼，确认该翻的补进 strings.json 或 overrides.json）")


if __name__ == "__main__":
    main()
