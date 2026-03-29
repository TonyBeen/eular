## Design Summary

本变更将错误语义分为两层：
- 内部实现层：允许保留历史返回值以降低重构风险。
- 对外 API 层：强制归一化为 POSIX 风格（失败 `-1` + `GetLastError()`）。

## Scope

- `Context` 对外接口：`bind/connect/connect0Rtt/connect0RttWithState/accept`
- `Connection` 对外接口：至少覆盖 `createStream` 失败路径
- `Stream` 对外接口：`write/read/commitWrite/consumeRead/reset/setPriority` 的失败路径

## Normalization Rules

1. 成功
- 状态型接口成功返回 `0`。
- 值型接口返回正向值（例如 `read` 返回读取字节数，EOF 返回 `0`）。

2. 失败
- 统一返回 `-1`。
- 在返回前必须设置 `SetLastErrorV(UTP_ERR_*, ...)`，并保证 `GetLastError()` 可观测。

3. 历史风格兼容
- 若内部返回 `-UTP_ERR_*` 或 `UTP_ERR_*`，公共包装层负责归一化并补齐 last-error。

## Non-goals

- 不修改协议帧、状态机线行为。
- 不改变 `stream id`、`read EOF` 等成功返回语义。
