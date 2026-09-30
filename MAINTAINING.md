# 维护说明

> 面向**维护这个汉化仓库的人**。想装这个 mod 的普通用户请回到 [README.md](README.md)。

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

**触发方式**（工作流没有定时任务，只在下面两种情况跑）：

| 触发 | 什么时候跑 |
| --- | --- |
| `push` | 改动 `dict/` `scripts/` `overlay/` 或 `localize.yml` 本身时自动跑一遍 |
| `workflow_dispatch` | 在 Actions 页面点 **Run workflow** 手动跑，可选 `release_type` 和 `skip_build` |

发布类型默认 `auto`：上游发了新正式版就发对应的中文正式版，否则发当日 nightly。

绝大多数时候是零人工：

- 上游改了别处 → CI 自动重放 → 编译 → 发 Release
- 上游新增了界面字符串 → `scan_strings.py` 扫出来 → `translate.py` 自动翻译 → 写回词典
- 上游改了我们动过的那段代码 → **CI 变红**，改 `dict/structural.json` 里的锚点即可

需要人工看的两件事：

| 情况 | 去哪看 | 怎么处理 |
| --- | --- | --- |
| CI 报「结构锚点未命中」 | Actions 日志里的 `precheck` 步骤 | 按日志给的 rule id 改 `dict/structural.json` |
| `dict/untranslated.json` 攒了条目 | 该文件 | 该翻的补进 `strings.json`，该保持英文的补进 `overrides.json` |

## 发布规则

`workflow_dispatch` 的 `release_type` 默认 `auto`：

- **正式版** —— 上游 fork 发了新正式版 tag，且本仓库还没发过 → tag 直接用上游那个
  完整 tag（如 `v0.1.24-colour-encoding-and-tune-fixes`），标题 `$TAG 简体中文版`
- **nightly** —— 其余情况发当日 `nightly-YYYYMMDD`，标记为 pre-release；
  同日重复触发自动加 `-2` / `-3` 后缀

判定用的是 `NR_RELEASE_MAJOR/MINOR/HOTFIX_VERSION` —— 这是**这个 fork 自己的发布号**。
同一个文件里的 `VER_MAJOR/MINOR/HOTFIX_VERSION` 是**冻结的上游同步标记**（长期停在
`0.7.7`，还被 XeSS/FSR 包装层拿去对游戏伪装引擎版本），拿它当版本号会永远算错。
老版本没有 `NR_RELEASE_*` 时才回退到 `VER_*`。

**正式版绝不覆盖**：同 tag 的 Release 已存在时直接报错退出，不会静默覆盖已发布的包。

## 发布包内容

包体是 `x64\Release\a\` 整目录（上游 Release 后置事件已经把运行库、`Licenses\`、
setup 脚本都拷进去了），再加上几项来自仓库根的文件，然后 `7z -mx=9` 打包。

`scripts/build.ps1` 负责三件事，缺一项包就不完整：

1. **剔除构建中间产物** —— `*.pdb` `*.lib` `*.exp` `*.ilk`。整目录照搬会把转发器的
   PDB 带出去，那里面有完整符号表和构建机的源码路径，既没用又泄漏路径。
   上游的 `package_release.ps1` 也是这么删的。
2. **补仓库根文件** —— `README.md` `INSTALL-DLSSNR.md` `LICENSE` `get_streamline.ps1`
   `docs\` `redist\`。这些不在编译输出目录里，只搬 `a\` 会漏掉。
3. **生成 `SHA256SUMS.txt`** —— 相对路径用正斜杠、无 BOM 的 UTF-8。

**体积参照**：包约 54 MB。这不是「内容比上游少」——上游自己的 nightly 也是
`7z a -r` 打同一个 `a\` 目录，同样是 53.7 MB。差的是压缩算法：上游那个 126 MB 的
正式包用 `Compress-Archive`（ZIP/Deflate，32 KB 窗口、逐文件独立压缩），
而包里 78 MB 的 `libxess.dll`、40 MB 的 `amd_fidelityfx_framegeneration_dx12.dll`
这类签名 DLL 熵很高，Deflate 只能压掉 20~35%；`7z` 的 LZMA2 用大字典 + 固实块
跨文件找冗余，整体能压到 26%。**同样的 212 MB 原始内容，Deflate 出 131.8 MB，
LZMA2 出 54 MB。**

**已知缺项**：上游正式包里的 `Optional\nvngx.dll_dlssnr.dll`（1.31 MB，AMD/Intel
厂商中立后端）我们没带。它不由本仓库的解决方案产出——上游的 `package_release.ps1`
是通过 `-PortBackendDll <路径>` 从外部传进去的，而仓库里的 `package_release.yml`
并没有传这个参数，说明发布时是手工提供的。要补得先搞清楚它的构建方式。

## 目录结构

```
dict/
  strings.json          主词典 en -> zh（唯一需要长期维护的核心资产）
  overrides.json        按文件 + 所在行上下文分派的例外
  structural.json       代码/构建配置的结构性改动规则
  targets.json          字面量改写的作用范围（只碰 menu/ 和 dlssnr/）
  glossary.json         术语表 + 文风要求，喂给翻译模型
  accepted_diffs.json   与参考汉化版「已知且已接受」的差异，供 verify.py 摘除告警
  untranslated.json     判定「保持英文」的串，附理由（自动生成 + 人工补）。
                        scan_strings.py 会把它算作已知，否则这些串每轮都会被重新扫出来
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
  apply_new_strings.py  把一批新串/改写串一次性补进词典（人工译好后的落盘工具）
.github/workflows/localize.yml
.upstream-revision      基线：上次同步到的上游 SHA（自动生成）
.upstream-files         上次同步放进工作区的上游文件清单（自动生成，gitignore）
```

## 本地跑一遍

```bash
# 1. 同步上游（预检/扫描够用；要编译就加 SYNC_SUBMODULES=1，
#    否则 external/ 下的 10 个 git 子模块是空的，MSBuild 必失败）
SYNC_SUBMODULES=1 bash scripts/sync_upstream.sh https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass.git main

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

**工作区是「构建沙箱」，不是「工作目录」**
`sync_upstream.sh` 跑完以后，工作区里除了白名单资产，其余都是上游源码，
随时会被下一次同步覆盖。所以别在这个目录里放东西。脚本清理时只认两样：

- `.github/` 下凡「出现在上游 tree 里」的文件一律删（白名单只有 `localize.yml`）
- 上次同步写下的 `.upstream-files` 清单里、这次上游已经没有的文件，删

两条都不涉及「工作区里凡不在上游 tree 的就删」——那条会把用户自己的本地文件
一起删掉。也正因为判据是「在上游 tree 里」而不是「git 跟踪的」，被误提交的上游
文件才拦得住：误提交的文件同样是被跟踪的。

**为什么要单独管 git 子模块**
上游有 10 个 git 子模块（`external/simpleini`、`spdlog`、`vulkan`、
`FidelityFX-SDK`、`nvapi`……）。`git checkout <tree> -- .` 只会把 gitlink
写进索引，**不会**把内容拉下来 —— 缺一个都编译不过。所以编译前要
`SYNC_SUBMODULES=1`，脚本会在 `git reset` 之前跑 `git submodule update --init
--recursive --depth 1`（必须在 reset 之前：reset 会把 gitlink 从索引里清掉，
之后就找不到要拉哪个 commit 了）。

预检和扫描不需要子模块，所以那两步不开这个开关，省几分钟。

## 已知的取舍

- `scan_strings.py` 会漏报「单个词」的界面标签（如 `Upscaler`），换取的是不把 DLL 名、
  枚举名、内部标识符误报成文案。当前词典已覆盖现有界面，新增长句仍能扫到。
- `translate.py` 拿不准的串一律走 SKIP 进 `untranslated.json`，不猜。
  宁可留一条英文，也不要翻错一个功能串。
- `sync_upstream.sh` 同步后不会自动处理上游的 rebase / force-push；
  基线记录在 `.upstream-revision` 里，出问题可以对着查。
- 首次运行时没有 `.upstream-files`，脚本会跳过「上游已移除」清理（正常，无副作用）。
- `sync_upstream.sh` 的退出码：0=有变化、1=无变化、3=出错。CI 里必须区分
  后两者——早期用 `|| true` 把错误和「无变化」一起吞了，子模块没拉下来也照样
  往下走，最后红在 MSBuild 上，日志里看不出根因。
- 本仓库的 git 历史里还留着早期 fork 带进来的上游源码。不影响使用，
  但仓库体积偏大；真要清得用 `filter-repo` 重写历史 + force push，代价是丢掉 fork 关系。
