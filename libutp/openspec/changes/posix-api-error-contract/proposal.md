## Why

当前 libutp 对外 API 存在两类失败返回风格混用：
- 直接返回 `-UTP_ERR_*`
- 返回正向 `UTP_ERR_*`

这会导致调用方无法按 POSIX 习惯统一处理错误，并增加跨语言封装成本。

## What Changes

- 冻结对外错误返回约束：失败统一返回 `-1`。
- 对外 API 失败时必须通过 `GetLastError()` 暴露具体 `UTP_ERR_*`。
- 成功返回维持原有语义：
  - 状态型接口成功返回 `0`。
  - 数据/对象型接口继续返回正向业务值（如字节数、stream id）。
- 清理遗留兼容路径：若内部仍返回历史风格，公共包装层必须归一化为 POSIX 风格。

## First-Release Rule

该约束按首版直接定型，不预留多套错误返回模式切换。

## Capabilities

### New Capabilities
- `public-api-error-semantics`: 统一对外 API 错误返回与 errno 语义。

## Impact

影响 `Context`、`Connection`、`Stream` 的对外行为一致性、测试断言与文档说明。该变更不引入协议线格式变化。
