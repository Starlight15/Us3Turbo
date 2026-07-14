# Us3Turbo

GDS (GPUDirect Storage) client for US3 Turbo — GPU-direct PUT via NVIDIA cuObject RDMA tokens.

## 代码格式化

仓库根的 `.clang-format` 固化了既有 Google 风格（2 空格缩进、80 列、左贴指针、
include 四段分组）。安装工具：

```bash
pip install --break-system-packages clang-format==18.1.8
clang-format --version   # 须为 18.x
```

格式化单个文件：

```bash
clang-format -i path/to/file.cpp
```

更推荐用封装脚本 `scripts/fmt-changed.sh`（参数与用法见脚本头部中文注释）：

```bash
# 格式化当前改动到的 C++ 文件（默认对比 HEAD，直接改写文件）
scripts/fmt-changed.sh
# 只检查不改动（提交前自查 / CI 门禁用同款命令）
scripts/fmt-changed.sh --check
# 检查整条分支相对 main 的格式
scripts/fmt-changed.sh --check --base origin/main
```

只读检查的手写等价命令（CI 增量门禁用同款）：

```bash
# 仅检查改动到的 C++ 文件
git diff --name-only origin/main...HEAD \
  | grep -E '\.(cpp|h)$' \
  | xargs -r clang-format --dry-run --Werror
```

**不批量格式化存量代码**，按文件跟着功能改动顺手格式化。

### blame 保护

`.git-blame-ignore-revs` 用于在未来一次性全量格式化时，让 `git blame` 跳过那条
重排 commit。本地启用（每位开发者执行一次）：

```bash
git config blame.ignoreRevsFile .git-blame-ignore-revs
```
