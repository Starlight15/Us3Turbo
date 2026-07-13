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

只读检查（不改动文件，CI 增量门禁用同款命令）：

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
