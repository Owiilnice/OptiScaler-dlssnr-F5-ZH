#!/usr/bin/env python3
"""回归测试：sync_upstream.sh 第 3 步的清理逻辑。

守的是一条很容易踩坏的不变量：

  「上游删掉的文件要被清掉」和「我们自己的同名文件不能被清掉」必须同时成立。

README.md 就是那个边界情况 —— 上游**也有**这个文件名，但它被 KEEP 排除，
工作区里躺的是我们那份中文说明，于是它同时满足
「在 .upstream-files 清单里」+「不在上游 tree 里」，正好落进「上游已移除」的判据。
没有 is_ours() 守卫的话，上游哪天删掉 README.md，我们的资产就跟着没了。

做法：真建一个临时 git 仓库，造出上面那个局面，跑一遍从 sync_upstream.sh
里抽出来的清理代码，检查三个文件的生死。

用法:
    python tools/test_sync_cleanup.py
退出码 0 = 通过，1 = 失败。
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCRIPT = os.path.join(REPO, "scripts", "sync_upstream.sh")


def extract_cleanup(src):
    m = re.search(r"run_py \"清理上游残留\".*?<<'PY'\n(.*?)\nPY\n", src, re.S)
    if not m:
        sys.exit("从 sync_upstream.sh 里找不到「清理上游残留」那段 python —— "
                 "脚本被改过？本测试需要跟着更新。")
    return m.group(1)


def main():
    sim = os.path.join(tempfile.gettempdir(), "synctest-%d" % time.time())
    os.makedirs(sim)
    try:
        return run(sim)
    finally:
        shutil.rmtree(sim, ignore_errors=True)


def run(sim):
    with open(SCRIPT, encoding="utf-8") as f:
        block = extract_cleanup(f.read())
    with open(os.path.join(sim, "cleanup.py"), "w", encoding="utf-8", newline="\n") as f:
        f.write(block)

    def git(*args):
        return subprocess.run(["git"] + list(args), cwd=sim,
                              capture_output=True, check=True)

    def write(rel, text="x"):
        p = os.path.join(sim, rel.replace("/", os.sep))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as f:
            f.write(text)

    git("init", "-q")
    git("config", "user.email", "test@example.invalid")
    git("config", "user.name", "test")

    # 上游原本有这三个文件；README.md 在真实仓库里是我们的资产
    write("README.md", "我们的中文说明")
    write("docs/old.md")
    write("docs/keep.md")
    git("add", "-A")
    git("commit", "-q", "-m", "s1")

    # 上游后来删掉了 README.md 和 docs/old.md。
    # 用 `git rm --cached` 从索引摘掉、文件留在磁盘上 —— 这才是真实状态：
    # 第 2 步的 checkout 只写不删，上游删掉的文件会一直躺在工作区里。
    git("rm", "-q", "--cached", "README.md", "docs/old.md")
    git("commit", "-q", "-m", "s2")
    sha2 = git("rev-parse", "HEAD").stdout.decode().strip()

    # .upstream-files 记的是上一次同步时工作区里有的文件
    with open(os.path.join(sim, ".upstream-files"), "w", encoding="utf-8", newline="\n") as f:
        f.write("README.md\ndocs/old.md\ndocs/keep.md\n")

    r = subprocess.run([sys.executable, "cleanup.py", sha2], cwd=sim,
                       capture_output=True, text=True, encoding="utf-8")
    print("--- 清理逻辑输出 ---")
    print((r.stdout or "").strip())
    if r.stderr:
        print("STDERR:", r.stderr.strip())

    alive = lambda rel: os.path.isfile(os.path.join(sim, rel.replace("/", os.sep)))
    checks = [
        ("README.md 存活（我们的资产，不能被当成上游残留删掉）", alive("README.md") is True),
        ("docs/old.md 已删（上游真的删了它）", alive("docs/old.md") is False),
        ("docs/keep.md 存活（上游还有它）", alive("docs/keep.md") is True),
    ]

    print("--- 结果 ---")
    ok = True
    for label, passed in checks:
        print("  [%s] %s" % ("通过" if passed else "失败", label))
        ok = ok and passed
    print()
    print("结论:", "通过" if ok else "失败")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
