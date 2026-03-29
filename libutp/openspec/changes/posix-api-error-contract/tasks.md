## 1. API Contract Freeze

- [x] 1.1 明确并记录对外 API 失败返回规则：统一 `-1`，错误码通过 `GetLastError()` 获取。
- [x] 1.2 识别当前混用路径（`-UTP_ERR_*` 与 `UTP_ERR_*`）并定义归一化策略。

## 2. Implementation

- [x] 2.1 `Context` 公共包装层增加状态归一化，屏蔽内部历史返回风格。
- [x] 2.2 `Connection::createStream` 失败路径改为 `-1` 并设置 last-error。
- [x] 2.3 `Stream` 对外失败路径改为 `-1` 并设置 last-error。

## 3. Validation

- [x] 3.1 更新现有单测断言为 POSIX 风格（返回 `-1` + `GetLastError()`）。
- [x] 3.2 新增公共 API 归一化用例（覆盖 `accept WOULD_BLOCK` 与 `connect0Rtt` 非法参数）。
- [x] 3.3 执行相关测试集并确认无回归。
