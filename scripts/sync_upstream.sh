#!/usr/bin/env bash
# 把上游源码同步进工作区，同时保证汉化资产不被覆盖、不被污染。
#
# 核心约定：上游源码**不进本仓库的版本库**。
#   本仓库只跟踪汉化资产（dict/ overlay/ scripts/ tools/ .github/workflows/localize.yml）。
#   每次跑 CI 时把上游整棵取到工作区，套用汉化、编译，用完即弃。
#   于是「汉化」和「上游」之间不存在 merge 冲突，只有目录分界。
#
# 用法:
#   scripts/sync_upstream.sh <上游仓库URL> <上游分支> [上游 commit SHA]
#
# 传 SHA 时按 SHA 取，保证「编译的代码 == 预检通过的代码」。
# 不传则取分支最新。
#
# 退出码:
#   0 = 上游代码有变化（需要重新编译）
#   1 = 上游代码与上次相同（可跳过）

set -euo pipefail

UPSTREAM_URL="${1:?用法: sync_upstream.sh <上游仓库URL> <上游分支> [SHA]}"
UPSTREAM_BRANCH="${2:?缺上游分支名}"
UPSTREAM_SHA="${3:-}"

# ubuntu runner 上 python 可能只叫 python3，windows runner 上叫 python
PY=""
for c in python3 python; do
  if command -v "$c" >/dev/null 2>&1; then PY="$c"; break; fi
done
[ -n "$PY" ] || { echo "找不到 python"; exit 1; }

# 本仓库自己的资产，绝不允许被上游覆盖。
# .gitignore / .gitattributes 也在这里：它们会直接影响 git 对「哪些文件算我们的资产」的判断，
# 让上游的版本盖过来风险太大（上游 .gitignore 里任何一条命中 dict/ scripts/ 就静默丢资产）。
KEEP=(
  ":(exclude).github"
  ":(exclude)overlay"
  ":(exclude)scripts"
  ":(exclude)dict"
  ":(exclude)tools"
  ":(exclude)README-ZH.md"
  ":(exclude).gitignore"
  ":(exclude).gitattributes"
  ":(exclude).upstream-files"
)
ASSETS=(dict overlay scripts tools .github README-ZH.md .gitattributes .gitignore .upstream-revision)

PREV_SHA=""
if [ -f .upstream-revision ]; then
  PREV_SHA=$(grep -m1 '^commit=' .upstream-revision | cut -d= -f2 || true)
fi

git remote remove upstream 2>/dev/null || true
git remote add upstream "$UPSTREAM_URL"

# 1) 取上游。优先按 SHA 取（可复现），取不到再退回分支头。
HEAD_SHA=""
if [ -n "$UPSTREAM_SHA" ]; then
  if git fetch upstream "$UPSTREAM_SHA" --depth 1 2>/dev/null; then
    HEAD_SHA="$UPSTREAM_SHA"
    echo "按 SHA 取到上游 $HEAD_SHA"
  else
    echo "按 SHA 取失败，退回分支头"
  fi
fi
if [ -z "$HEAD_SHA" ]; then
  git fetch upstream "$UPSTREAM_BRANCH" --depth 1
  HEAD_SHA=$(git rev-parse FETCH_HEAD)
  echo "上游 $UPSTREAM_BRANCH 最新 = $HEAD_SHA"
fi

# 2) 整棵覆盖上游源码到工作区（KEEP 里的路径不动）
git checkout "$HEAD_SHA" -- . "${KEEP[@]}"

# 3) 清理。
#
#    git checkout 只会「写」和「覆盖」，永远不会「删」。所以两件事得自己干：
#
#    a) 上游删掉的源文件，工作区还留着 —— 用 .upstream-files（上次同步放进来的
#       文件清单）和新上游 tree 求差集。用清单而不是「工作区里凡不在上游 tree 的
#       文件」，是因为后者会把用户自己的本地文件一起删掉。
#
#    b) 上游**有**、但落在 KEEP 目录里的文件（典型：.github/ISSUE_TEMPLATE、
#       .github/workflows/build.yml）—— 第 2 步的 pathspec 把它们排除在覆盖之外，
#       (a) 也管不到（上游确实有它们）。它们一旦进了工作区就永久残留，
#       下一次提交就被带进仓库。这就是本仓库曾经混进 7 个上游 workflow 的原因。
#
#       判据必须是「在上游 tree 里」而不是「git 跟踪的」—— 被误提交的上游文件
#       同样是被跟踪的，用后者当判据等于什么都没判（第一版修复就栽在这里）。
"$PY" - "$HEAD_SHA" <<'PY'
import os, subprocess, sys

sha = sys.argv[1]

def ls_tree(args):
    # 必须用 -z：不加的话 git 会给含空格/非 ASCII 的路径套上双引号并转义，
    # 于是 "docs/DLSS-NR Enlarge Paths.html" 匹配不上工作区里的裸路径，
    # 被误判成「上游已删除」而删掉 —— 上游确实有这种带空格的文件。
    raw = subprocess.run(["git", "ls-tree", "-r", "-z", "--name-only"] + args,
                         capture_output=True, check=True).stdout
    return {p.decode("utf-8", "surrogateescape") for p in raw.split(b"\0") if p}

up = ls_tree([sha])

# 我们自己的地盘。.github/ 只认白名单里那一个文件，其余一律不认。
OURS_EXACT = {
    "README-ZH.md", ".gitattributes", ".gitignore", ".upstream-revision",
    ".github/workflows/localize.yml",
}
OURS_DIRS = ("dict/", "overlay/", "scripts/", "tools/")

prev = None
if os.path.isfile(".upstream-files"):
    with open(".upstream-files", encoding="utf-8") as f:
        prev = {ln.strip() for ln in f if ln.strip()}

stale = []

# a) 上游已移除
if prev is None:
    print("没有 .upstream-files（首次运行），跳过「上游已移除」清理")
else:
    stale.extend(sorted(prev - up))

# b) .github/ 里的上游文件
for root, dirs, files in os.walk(".github"):
    for f in files:
        p = os.path.join(root, f).replace("\\", "/")
        if p in OURS_EXACT:
            continue
        if p in up:
            stale.append(p)

# 去重、去掉已经不存在的
seen = set()
todo = []
for p in stale:
    if p in seen or not os.path.isfile(p):
        continue
    seen.add(p)
    todo.append(p)

if todo:
    print(f"清理 {len(todo)} 个上游残留文件:")
    for p in todo:
        print(f"    {p}")
    subprocess.run(["git", "rm", "-q", "-f", "--cached", "--ignore-unmatch", "--"] + todo,
                   check=False)
    for p in todo:
        try:
            os.remove(p)
        except OSError:
            pass
    # 顺手清掉可能空掉的目录
    for p in todo:
        d = os.path.dirname(p)
        while d and d not in (".", ""):
            try:
                os.rmdir(d)
            except OSError:
                break
            d = os.path.dirname(d)
else:
    print("无上游残留文件")
PY

# 4) 记录基线 + 本次放进工作区的上游文件清单
{
  echo "commit=$HEAD_SHA"
  echo "upstream=$UPSTREAM_URL"
  echo "branch=$UPSTREAM_BRANCH"
  echo "synced_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > .upstream-revision

# 清单只记「真的落在工作区」的文件，下次靠它算差集
git ls-tree -r -z --name-only "$HEAD_SHA" | "$PY" -c '
import os, sys
data = sys.stdin.buffer.read()
with open(".upstream-files", "w", encoding="utf-8", newline="\n") as f:
    for p in data.split(b"\0"):
        if not p:
            continue
        s = p.decode("utf-8", "surrogateescape")
        if os.path.isfile(s):
            f.write(s + "\n")
'

# 5) 只把汉化资产放进暂存区。
#    git checkout <tree> -- <paths> 会同时改索引，所以先 reset 把上游源码摘出去，
#    否则一次 CI 就会把整个上游源码树提交进仓库 —— 那就退化回 merge 冲突的老路了。
#    -A 是必需的：第 3 步删掉的残留文件也要把删除入账。
if git rev-parse --verify HEAD >/dev/null 2>&1; then
  git reset -q
fi
git add -A -- "${ASSETS[@]}" 2>/dev/null || true

if [ -n "$PREV_SHA" ] && [ "$PREV_SHA" = "$HEAD_SHA" ]; then
  echo "上游无变化（$HEAD_SHA）"
  exit 1
fi
echo "上游有变化: ${PREV_SHA:-<首次>} -> $HEAD_SHA"
exit 0
