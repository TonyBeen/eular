# STUN 文档索引

这里统一收纳 `stun` 相关设计、部署和开发任务文档。

## 文档列表

- [多机器协作STUN服务设计方案](./多机器协作STUN服务设计方案.md)
- [STUN Hub与STUN节点协同架构设计](./STUNHub与STUN节点协同架构设计.md)
- [STUN Hub 的 Mosquitto 部署文档](./STUN%20Hub的Mosquitto部署文档.md)
- [STUN双机最小部署指南](./STUN双机最小部署指南.md)
- [NAT类型与探测信号说明](./NAT类型与探测信号说明.md)
- [STUN NAT探测流程图](./STUN_NAT探测流程图.md)
- [STUN开发任务单](./开发任务单.md)

## 构建补充

- 默认构建：`cmake --preset default && cmake --build --preset default`
- musl 静态构建：`cmake --preset musl && cmake --build --preset musl`
- musl 工具链目录优先读取 `STUN_MUSL_ROOT`，兼容 `MUSL_ROOT` 和 `MUSL_PATH` 环境变量，默认 `/opt/musl-toolchain`

## 当前实现补充

- NAT 探测主路径已经切换为完整异步 full STUN 流程：
  - `stun1`
  - `CHANGE-REQUEST(change-port)`
  - `CHANGE-REQUEST(change-ip)`
  - `stun2`
- `stun_peer` 与 `stun_nat_detect` 复用同一套异步 NAT probe flow，不再各自维护独立探测逻辑。
- 会话协商除公网 `srflx` 候选外，还会交换 `host_local` 候选；同机或同局域网场景会优先命中本地地址。
- 候选列表会自动去重；当 `srflx_primary == srflx_secondary` 时，不再重复下发第二条公网候选。
- 控制面协议热点路径已经基本切到 `FieldTag + typed TLV`，数值和枚举字段优先走二进制编码与按 tag 访问。
