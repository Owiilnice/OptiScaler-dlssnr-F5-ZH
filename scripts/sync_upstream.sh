#!/usr/bin/env bash
# 把上游源码同步进工作区，同时保证汉化资产不被覆盖。
#
# 核心约定：上游源码**不进本仓库的版本库**。
#   本仓库只跟踪汉化资产（dict/ overlay/ scripts/ tools/ .github/ README-ZH.md）。
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

# 3) 删掉上游已删除、工作区还留着的文件。
#    不做这步，上游删掉一个源文件后构建会因为残留文件而失败。
"$PY" - "$HEAD_SHA" <<'PY'
import os, subprocess, sys

sha = sys.argv[1]
# 必须用 -z：不加的话 git 会给含空格/非 ASCII 的路径套上双引号并转义，
# 于是 "docs/DLSS-NR Enlarge Paths.html" 匹配不上工作区里的裸路径，
# 被误判成「上游已删除」而删掉 —— 上游确实有这种带空格的文件。
raw = subprocess.run(["git", "ls-tree", "-r", "-z", "--name-only", sha],
                     capture_output=True, check=True).stdout
up = {p.decode("utf-8", "surrogateescape") for p in raw.split(b"\0") if p}
# 本仓库自己的东西，永远不删
keep = (".github/", "overlay/", "scripts/", "dict/", "tools/", "README-ZH.md",
        ".gitattributes", ".gitignore", ".upstream-revision")
stale = []
for root, dirs, files in os.walk("."):
    dirs[:] = [d for d in dirs if d not in (".git", "dist")]
    for f in files:
        p = os.path.relpath(os.path.join(root, f), ".").replace("\\", "/")
        if p in keep or p.startswith(keep):
            continue
        if p not in up:
            stale.append(p)
if stale:
    print(f"删除上游已移除的 {len(stale)} 个文件:")
    for p in sorted(stale)[:20]:
        print(f"    {p}")
    subprocess.run(["git", "rm", "-q", "-f", "--cached", "--ignore-unmatch", "--"] + stale, check=False)
    for p in stale:
        try:
            os.remove(p)
        except OSError:
            pass
PY

# 4) 记录基线：编译时按这个 SHA 取，保证和预检的是同一份代码
{
  echo "commit=$HEAD_SHA"
  echo "upstream=$UPSTREAM_URL"
  echo "branch=$UPSTREAM_BRANCH"
  echo "synced_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > .upstream-revision

# 5) 只把汉化资产放进暂存区。
#    git checkout <tree> -- <paths> 会同时改索引，所以先 reset 把上游源码摘出去，
#    否则一次 CI 就会把整个上游源码树提交进仓库 —— 那就退化回 merge 冲突的老路了。
if git rev-parse --verify HEAD >/dev/null 2>&1; then
  git reset -q
fi
git add -- "${ASSETS[@]}" 2>/dev/null || true

if [ -n "$PREV_SHA" ] && [ "$PREV_SHA" = "$HEAD_SHA" ]; then
  echo "上游无变化（$HEAD_SHA）"
  exit 1
fi
echo "上游有变化: ${PREV_SHA:-<首次>} -> $HEAD_SHA"
exit 0
