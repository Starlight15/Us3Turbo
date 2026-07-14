#!/usr/bin/env bash
#
# fmt-changed.sh —— 格式化本次改动到的 C++ 文件
#
# 使用方法（在仓库根目录执行）：
#
#   # 1) 默认：格式化你正在编辑的改动（工作区相对 HEAD 的 .cpp/.h，直接改写文件）
#   scripts/fmt-changed.sh
#
#   # 2) 只检查、不改动文件（提交前自查 / CI 门禁用同款命令）
#   scripts/fmt-changed.sh --check
#
#   # 3) 指定对比基线（默认 HEAD，即未提交的改动）
#   #    检查整条分支相对 main 的格式（CI 用）：
#   scripts/fmt-changed.sh --check --base origin/main
#   #    只看最近 5 个 commit 引入的改动：
#   scripts/fmt-changed.sh --base HEAD~5
#
#   # 4) 直接格式化指定文件（不查 git diff，新文件常用）
#   scripts/fmt-changed.sh path/to/new_file.cpp
#
# 退出码：
#   0 —— 全部文件已合规（--check 模式下表示无格式问题）
#   非0 —— 有文件被改动，或 --check 模式下发现格式问题，或工具缺失
#
# 说明：
#   - 不批量格式化存量代码，只处理本次功能改动触及的文件，避免制造淹没
#     git blame 历史的大重排 commit。
#   - 必须使用 clang-format 18.x，不同大版本对续行/include 分组的处理
#     不一致，混用会反复打架。
#   - 新增的未跟踪文件不会出现在 `git diff` 里：先 `git add -N <file>`
#     （标记为“意图添加”，不暂存内容）再运行本脚本，或直接把文件名作为
#     参数传入。
#   - 全量格式化的那条 commit 已记入 .git-blame-ignore-revs，本地执行一次
#     `git config blame.ignoreRevsFile .git-blame-ignore-revs` 即可让
#     git blame 跳过它。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${PROJECT_ROOT}"

# ---- 解析参数 ----
CHECK=0
BASE="HEAD"
EXPLICIT_FILES=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --check) CHECK=1; shift ;;
    --base)  BASE="$2"; shift 2 ;;
    -h|--help)
      sed -n '2,40p' "${BASH_SOURCE[0]}"
      exit 0 ;;
    --) shift; EXPLICIT_FILES+=("$@"); break ;;
    -*) echo "未知参数: $1" >&2; exit 2 ;;
    *)  EXPLICIT_FILES+=("$1"); shift ;;
  esac
done

# ---- 前置检查：clang-format 须存在且为 18.x ----
if ! command -v clang-format >/dev/null 2>&1; then
  echo "错误：未找到 clang-format。安装：pip install --break-system-packages clang-format==18.1.8" >&2
  exit 1
fi
CF_MAJOR="$(clang-format --version 2>/dev/null | grep -oE 'version [0-9]+' | head -1 | grep -oE '[0-9]+$')"
if [[ "${CF_MAJOR}" != "18" ]]; then
  echo "错误：clang-format 版本须为 18.x，当前为 $(clang-format --version)" >&2
  exit 1
fi

# ---- 收集待处理文件 ----
# 给出显式文件 → 只处理它们；否则取工作区相对 BASE 改动到的 C++ 文件。
# --diff-filter=d 过滤掉已删除的文件，避免对不存在的路径运行 clang-format。
if [[ ${#EXPLICIT_FILES[@]} -gt 0 ]]; then
  FILES=("${EXPLICIT_FILES[@]}")
else
  mapfile -t FILES < <(
    git diff --name-only --diff-filter=d "${BASE}" 2>/dev/null \
      | grep -E '\.(cpp|h|hpp|cc|c)$' || true
  )
fi

if [[ ${#FILES[@]} -eq 0 ]]; then
  echo "没有相对 ${BASE} 改动到的 C++ 文件，无需格式化。"
  exit 0
fi

echo "待处理文件（基线 ${BASE}）："
printf '  %s\n' "${FILES[@]}"
echo

if [[ "${CHECK}" -eq 1 ]]; then
  # 只读检查：--dry-run 不改文件，--Werror 使有差异时返回非 0
  clang-format --dry-run --Werror "${FILES[@]}"
  echo "检查通过：所有改动文件均已合规。"
else
  # 直接改写文件
  clang-format -i "${FILES[@]}"
  echo "已格式化上述文件。请用 git diff 复核后提交。"
fi
