# OptiScaler-F5-DLSSNR-Multipass 简体中文汉化（可持续维护版）

上游 [janblade/OptiScaler-F5-DLSSNR-Multipass](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass)
更新很勤。这个仓库解决的是**「每次上游更新，汉化就得重做一遍」**的问题。

## 核心思路

汉化不再是「改源码」，而是**一套可以反复套用的资产**：

| 资产 | 文件 | 上游更新后 |
| --- | --- | --- |
| 英译中词典 | `dict/strings.json` | 不动。锚点是英文原文，不是行号 |
| 上下文例外 | `dict/overrides.json` | 不动 |
| 代码结构改动 | `dict/structural.json` | 锚点是原文片段，上游挪位置不影响 |
| 新增文件 | `overlay/` | 不动（字形范围表、字体） |
| 作用范围 | `dict/targets.json` | 不动 |

上游源码**永远不进这个仓库**。CI 每次从上游整棵取出来覆盖，然后当场套用上面这套资产再编译。
于是「汉化」和「上游」之间根本不存在 merge 冲突——只有「我们的目录」和「上游路径」的分界。

## 三条流水线

`scripts/apply_patch.py` 按顺序跑三步，全部幂等（重复跑不会二次替换）：

1. **结构规则** — 上游原文片段作锚点，替换成汉化版代码。锚点是空白不敏感的，缩进怎么变都能命中。
2. **字面量改写** — 用 C 字符串扫描器切出「字面量组」（相邻字面量 `"a" "b"` 合成一个逻辑串），
   拿逻辑串去词典查。所以上游把多行 `HelpMarker` 折成一行、或拆成多行，都不影响命中。
3. **覆盖文件** — 把 `overlay/` 里的文件拷进源码树。

**锚点未命中 = 报错退出（exit 2），不是静默跳过。** 上游改了同一处代码时，CI 必须红，
而不是发一个半汉化的包出去。

## 日常维护

绝大多数时候是零人工：

- 上游改了别处 → CI 自动重放 → 编译 → 发 Release
- 上游新增了界面字符串 → `scan_strings.py` 扫出来 → `translate.py` 自动翻译 → 写回词典
- 上游改了我们动过的那段代码 → **CI 变红**，改 `dict/structural.json` 里的锚点即可

需要人工看的两件事：

| 情况 | 去哪看 | 怎么处理 |
| --- | --- | --- |
| CI 报「结构锚点未命中」 | Actions 日志里的 `precheck` 步骤 | 按日志给的 rule id 改 `dict/structural.json` |
| `dict/untranslated.json` 攒了条目 | 该文件 | 该翻的补进 `strings.json`，该保持英文的补进 `overrides.json` |

## 目录结构

```
dict/
  strings.json          主词典 en -> zh（唯一需要长期维护的核心资产）
  overrides.json        按文件 + 所在行上下文分派的例外
  structural.json       代码/构建配置的结构性改动规则
  targets.json          字面量改写的作用范围（只碰 menu/ 和 dlssnr/）
  glossary.json         术语表 + 文风要求，喂给翻译模型
  accepted_diffs.json   与参考汉化版「已知且已接受」的差异，供 verify.py 摘除告警
  untranslated.json     模型判定「不该翻」或拿不准的串，等人裁决（自动生成）
overlay/
  OptiScaler/menu/font/ChineseGlyphRanges.h   CJK 字形范围表
  font/wqy-microhei.ttc                       文泉驿微米黑（内置 Hack 无汉字字形）
scripts/
  apply_patch.py        重放器（结构规则 + 字面量 + 覆盖）
  sync_upstream.sh      同步上游源码，保留本仓库资产
  scan_strings.py       扫出未翻译的界面字符串
  translate.py          调大模型补译，产出 pending / untranslated
  verify.py             拿重放结果与参考汉化版对账
  build.ps1             MSBuild 编译 + 产物自检
tools/                  一次性工具，平时不用跑
  extract_dict.py       从「上游原文 vs 参考汉化版」抽取词典
  build_structural.py   抽取结构性改动规则
  probe.py              查某个英文串在两侧的全部出现处，用于判定该不该翻
.github/workflows/localize.yml
```

## 本地跑一遍

```bash
# 1. 同步上游
bash scripts/sync_upstream.sh https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass.git main

# 2. 重放汉化（--check 只检测不写盘）
python scripts/apply_patch.py . --check

# 3. 正式重放
python scripts/apply_patch.py . --report apply-report.json

# 4. 扫描新增字符串（有新增时退出码 3）
python scripts/scan_strings.py . --out pending.json --markdown pending.md

# 5. 自动翻译（需要 DEEPSEEK_API_KEY）
python scripts/translate.py --input pending.json

# 6. 与参考汉化版对账（需要一份已确认可用的汉化版源码树）
python scripts/verify.py <重放后的源码树> <参考汉化版源码树>
```

## 为什么这么设计

**为什么词典用「英文原文」而不是行号或地址做键？**
行号和地址每版都变。英文原文只在文案本身被改写时才变，那时本来也需要重译。

**为什么上游源码不入库？**
一旦入库，上游和汉化就在同一份文件里，`git merge` 必然冲突，冲突解决又必然出错。
分开之后，「上游路径」每次整体覆盖，「我们的目录」永远不动。

**为什么结构规则要报错而不是跳过？**
静默跳过会产出一个「大部分中文、局部英文」的包，而且没有任何信号。
报错会把问题钉在 CI 上，成本只是改一个锚点。

**为什么 `verify.py` 要留 `accepted_diffs.json`？**
参考汉化版是人工确认过能跑的，但不等于完美。已接受的差异单独记账，
任何**新增**差异都会让 CI 变红——这样上游的行为变化不会悄悄溜过去。

## 已知的取舍

- `scan_strings.py` 会漏报「单个词」的界面标签（如 `Upscaler`），换取的是不把 DLL 名、
  枚举名、内部标识符误报成文案。当前词典已覆盖现有界面，新增长句仍能扫到。
- `translate.py` 拿不准的串一律走 SKIP 进 `untranslated.json`，不猜。
  宁可留一条英文，也不要翻错一个功能串。
- `sync_upstream.sh` 同步后不会自动处理上游的 rebase / force-push；
  基线记录在 `.upstream-revision` 里，出问题可以对着查。
