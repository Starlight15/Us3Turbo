# Proxy 分层重构 — 阶段 1：建立 status.h + 目录迁移

**目标**：建立服务层错误类型 `ProxyStatus`，把接口层从 `service/` 迁到 `api/`，
**不改任何逻辑**——纯目录调整 + 新增 status.h，确保可编译可测。

**范围**：1 个新文件 + 2 个文件迁移 + CMakeLists.txt 更新。

---

## 改动 1：新增 proxy/src/common/status.h

```cpp
#pragma once
#include <string>
#include <utility>
#include "proxy/src/common/errors.h"

namespace us3_turbo::proxy {

// 服务层统一返回类型：code==0 成功，非 0 为 PROXY_ERR_*。
// 接口层据此填 response + cntl->SetFailed；不把 brpc 概念泄漏进服务层。
struct ProxyStatus {
  int         code{0};
  std::string message;

  [[nodiscard]] bool ok() const { return code == 0; }
  
  static ProxyStatus Ok() { return {}; }
  
  static ProxyStatus Fail(int code, std::string msg) {
    return {code, std::move(msg)};
  }
};

}  // namespace us3_turbo::proxy
```

---

## 改动 2：目录迁移（纯移动，不改内容）

```bash
mkdir -p proxy/src/api
git mv proxy/src/service/proxy_control_plane_service.h proxy/src/api/
git mv proxy/src/service/proxy_control_plane_service.cpp proxy/src/api/
```

**文件内容不变**，只改 `#include` 路径：
- `proxy_control_plane_service.cpp` 第 1 行：
  ```cpp
  #include "proxy/src/api/proxy_control_plane_service.h"  // 原 service/
  ```

---

## 改动 3：CMakeLists.txt 更新路径

`proxy/CMakeLists.txt` 第 7 行：
```cmake
src/api/proxy_control_plane_service.cpp  # 原 src/service/...
```

---

## 验收（阶段 1）

**编译**
```bash
cd build && ninja us3_turbo_proxy
```
应无警告通过。

**行为验证**
- [ ] 启动 proxy，单步 GdsPut/UcxPut 成功
- [ ] CreateMultipartUpload + UploadPartGds + CompleteMultipartUpload 成功
- [ ] 16MiB 边界、part 升序校验、final etag 全部不变

**文件检查**
- [ ] `proxy/src/common/status.h` 存在且可被 include
- [ ] `proxy/src/api/proxy_control_plane_service.*` 存在
- [ ] `proxy/src/service/proxy_control_plane_service.*` 已删除
- [ ] 编译无 "file not found" 错误

---

## 交付物

1. `common/status.h`（新）
2. `api/proxy_control_plane_service.{h,cpp}`（迁移自 service/，内容不变）
3. `CMakeLists.txt`（路径更新）
4. 行为等价确认（单步 + multipart 测试全绿）

完成后进入阶段 2（抽服务层 SinglePutService/MultipartService）。
