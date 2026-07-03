# STUN Hub 的 Mosquitto 部署文档

## 1. 作用

`stun_hub` 当前使用 Eclipse Mosquitto 作为 MQTT 控制面总线：

- `stun_node` 向 MQTT 发布节点注册、在线状态和心跳。
- `stun_hub` 订阅 Node 事件，维护集群视图。
- `stun_hub` 向每个 Node 发布 assignment，让 Node 自动发现其他协作 Node。
- Node 启动时只需要知道 Hub/MQTT 地址，不需要知道对端 Node。

## 2. 安装

Ubuntu/Debian：

```bash
sudo apt update
sudo apt install -y mosquitto mosquitto-clients
sudo systemctl enable --now mosquitto
sudo systemctl status mosquitto
```

确认监听：

```bash
sudo ss -lntp | grep 1883
sudo ss -lnup | grep mosquitto
```

## 3. 账号与密码

公网部署不建议允许匿名连接。先创建 STUN 专用用户：

```bash
sudo mosquitto_passwd -c /etc/mosquitto/passwd stun
```

如果密码文件已经存在，新增或重置用户时不要加 `-c`：

```bash
sudo mosquitto_passwd /etc/mosquitto/passwd stun
```

查看已有用户名：

```bash
sudo cut -d: -f1 /etc/mosquitto/passwd
```

注意：Mosquitto 密码文件保存的是 hash，不能查看明文密码，只能重置。

## 4. ACL

创建 ACL 文件：

```bash
sudo tee /etc/mosquitto/aclfile >/dev/null <<'EOF'
user stun
topic readwrite stun/#
EOF
```

最小权限可以后续再收敛。当前联调阶段先允许 `stun/#` 读写，避免 Hub 和 Node topic 权限不一致。

当前使用到的 topic：

```text
stun/node/+/register
stun/node/+/presence
stun/node/+/heartbeat
stun/hub/cluster/snapshot
stun/hub/cluster/events
stun/hub/node/+/assignment
```

## 5. Mosquitto 配置

不要在已有 Mosquitto 配置上直接新增一份完整 `stun.conf`。`listener`、`allow_anonymous`、`password_file`、`acl_file`、`persistence_location` 等全局项重复会导致 Mosquitto 启动失败。

先检查现有配置：

```bash
sudo grep -R -n "listener\|allow_anonymous\|password_file\|acl_file\|persistence_location\|include_dir" /etc/mosquitto
```

如果 `/etc/mosquitto/mosquitto.conf` 里已经有以下配置，只需要确认路径正确，不要再在 `conf.d` 里重复写：

```conf
listener 1883
allow_anonymous false
password_file /etc/mosquitto/passwd
acl_file /etc/mosquitto/aclfile
```

如果只缺 `acl_file`，优先把这一行加到主配置中已有认证配置附近：

```conf
acl_file /etc/mosquitto/aclfile
```

如果主配置很干净，才考虑新增一个最小配置文件。不要包含已经在主配置中存在的项：

```bash
sudo tee /etc/mosquitto/conf.d/stun.conf >/dev/null <<'EOF'
acl_file /etc/mosquitto/aclfile
EOF
```

配置原则：

- 同一个全局项只保留一处。
- 如果主配置已有 `listener`，不要在 `stun.conf` 重复写 `listener`。
- 如果主配置已有 `password_file`，不要在 `stun.conf` 重复写 `password_file`。
- 如果主配置已有 `persistence_location`，不要在 `stun.conf` 重复写 `persistence_location`。

重启服务：

```bash
sudo systemctl restart mosquitto.service
sudo systemctl status mosquitto.service
```

如果启动失败，查看日志：

```bash
sudo journalctl -u mosquitto.service -b --no-pager -n 80
```

如果看到类似下面的错误：

```text
Error: Duplicate password_file value in configuration.
Error: Duplicate persistence_location value in configuration.
```

说明配置重复。先禁用新增的 `stun.conf` 恢复服务：

```bash
sudo mv /etc/mosquitto/conf.d/stun.conf /etc/mosquitto/conf.d/stun.conf.disabled
sudo systemctl reset-failed mosquitto
sudo systemctl restart mosquitto
sudo systemctl status mosquitto
```

## 6. 连接验证

匿名连接应该失败：

```bash
mosquitto_sub -h <Hub域名或IP> -p 1883 -t 'stun/#' -C 1
```

预期类似：

```text
Connection error: Connection Refused: not authorised.
```

使用账号密码验证发布订阅。

终端 1：

```bash
mosquitto_sub -h <Hub域名或IP> -p 1883 -u stun -P '<密码>' -t 'stun/#' -v
```

终端 2：

```bash
mosquitto_pub -h <Hub域名或IP> -p 1883 -u stun -P '<密码>' -t 'stun/test' -m ok
```

终端 1 应收到：

```text
stun/test ok
```

## 7. 启动 STUN Hub

`stun_hub` 参数：

```bash
./stun_hub [mqtt_host] [mqtt_port] [mqtt_username] [mqtt_password]
./stun_hub --host <mqtt_host> [--port 1883] [--username <mqtt_username>] [--password <mqtt_password>]
```

示例：

```bash
./stun_hub --host bd.eular.top --username stun --password '<密码>'
```

位置参数方式仍兼容，但不推荐在中间省略参数，否则容易发生错位。优先使用 `--host/--port/--username/--password`。

正常日志应包含：

```text
Connected to MQTT broker
Subscribed to topic: stun/node/+/register
Subscribed to topic: stun/node/+/presence
Subscribed to topic: stun/node/+/heartbeat
Subscribed to topic: stun/hub/cluster/snapshot
stun_hub running broker=bd.eular.top:1883
```

如果出现：

```text
MQTT connect callback error: The connection was refused.
mosquitto_loop_read failed: The connection was refused.
```

说明 broker 拒绝连接，优先检查用户名、密码和 ACL。

## 8. 启动 STUN Node

Node 只需要知道 Hub/MQTT endpoint，不需要知道其他 Node。

Node-A：

```bash
./stun_node \
  --hub bd.eular.top:1883 \
  --node-id node-a \
  --public-host <NodeA公网IP或域名> \
  --control-port 19000 \
  --stun-port 3478 \
  --mqtt-username stun \
  --mqtt-password '<密码>'
```

Node-B：

```bash
./stun_node \
  --hub bd.eular.top:1883 \
  --node-id node-b \
  --public-host <NodeB公网IP或域名> \
  --control-port 19000 \
  --stun-port 3478 \
  --mqtt-username stun \
  --mqtt-password '<密码>'
```

Node 行为：

- 本地绑定 `19000/tcp`。
- 本地绑定 `3478/udp`。
- 向 Hub 注册 `<public-host>:19000` 和 `<public-host>:3478`。
- 从 Hub 接收 assignment，自动发现其他 Node。

## 9. 防火墙

Hub/MQTT 机器：

```text
1883/tcp
```

每台 STUN Node：

```text
19000/tcp
3478/udp
3479/udp
```

`3479/udp` 用于过滤探测辅助端口。当前 V4 跑通阶段建议一起放行。

## 10. 常见问题

### 10.1 匿名订阅失败

```text
Connection Refused: not authorised.
```

这是正常的，说明 `allow_anonymous false` 生效。使用 `-u/-P` 连接。

### 10.2 Hub 先显示 Connected，随后 refused

`Connected to MQTT broker` 只表示客户端发起连接成功，不代表 broker 已接受认证。真正结果以 MQTT connect callback 为准。

处理：

```bash
mosquitto_sub -h <host> -p 1883 -u stun -P '<密码>' -t 'stun/#' -C 1
```

如果仍失败，检查：

```bash
sudo grep -R "allow_anonymous\|password_file\|acl_file" /etc/mosquitto
sudo cut -d: -f1 /etc/mosquitto/passwd
sudo journalctl -u mosquitto.service -b --no-pager -n 80
```

### 10.3 Node 没收到 assignment

检查 Hub 是否收到 Node 注册：

```bash
mosquitto_sub -h <Hub> -p 1883 -u stun -P '<密码>' -t 'stun/#' -v
```

应看到：

```text
stun/node/node-a/register ...
stun/node/node-b/register ...
stun/hub/node/node-a/assignment ...
stun/hub/node/node-b/assignment ...
```

如果只有 register 没有 assignment，检查 `stun_hub` 是否运行。

## 11. 参考命令

查看 Mosquitto 配置：

```bash
sudo grep -R "listener\|allow_anonymous\|password_file\|acl_file\|include_dir" /etc/mosquitto
```

重载：

```bash
sudo systemctl reload mosquitto
```

重启：

```bash
sudo systemctl restart mosquitto
```

日志：

```bash
sudo journalctl -u mosquitto.service -f
```
