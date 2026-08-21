# NTRS 服务构建

在 `ntrs` 目录执行 CMake preset。相同 preset 始终复用同一构建目录中的 BoringSSL 和 libevent 中间产物。

```sh
cd ntrs
cmake --preset ntrs-linux
cmake --build --preset ntrs-linux
ctest --preset ntrs-linux
```

Linux 服务产物固定在 `../build/ntrs-linux/`：

- `nat_detect_hub`
- `nat_detect_node`

musl 构建使用 `ntrs-musl` preset，产物固定在 `../build/ntrs-musl/`。

服务端默认以明文 TCP 运行；同时传入 `--cert` 与 `--key` 时启用 TLS 1.3。Hub 默认监听
`0.0.0.0:24000`，Node 默认监听 `0.0.0.0:24001`、`0.0.0.0:24002`、`0.0.0.0:24003`：

```sh
../build/ntrs-linux/nat_detect_hub --interface eth0
../build/ntrs-linux/nat_detect_node --hub hub.example.com:24000 --node-id node-a --interface eth0
```

`--interface` 在 Linux 上通过 `SO_BINDTODEVICE` 绑定所有服务 socket；多线部署应指定它。Hub 从
Node 控制连接的源地址确定 Node 的服务 IP，因此 Node 只配置本地监听端口，不能上报或覆盖公网 IP。
