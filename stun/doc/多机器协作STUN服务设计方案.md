# ORION(Open Reliable Interconnect ONline) - 多机器协作STUN服务设计方案

## 1. 项目概述

### 1.1 背景
当前线上环境为单网卡机器，无法执行完整的NAT探测功能。需要设计一个支持多机器协作的ORION服务，通过多机器配合实现完整的NAT类型探测。

### 1.2 目标
- 实现支持多机器协作的ORION服务
- 从HTTPS拉取ORION服务列表
- 实现完整的NAT探测功能
- 提供高可用性和可扩展性

## 2. 系统架构

### 2.1 整体架构
```
+-------------------+       +-------------------+       +-------------------+
|   STUN Client     |<----->|   ORION Server    |<----->|   ORION Server    |
+-------------------+       +-------------------+       +-------------------+
                               |            ^
                               v            |
                          +-------------------+
                          | Service Discovery |
                          +-------------------+
                               |
                               v
                          +-------------------+
                          |  HTTPS API       |
                          +-------------------+
```

### 2.2 核心组件
- **ORION Server**：处理STUN请求，执行NAT探测
- **Service Discovery**：从HTTPS拉取ORION服务列表
- **Inter-Server Communication**：机器间通信协议
- **Coordinator**：协调多机器执行探测

## 3. 服务发现机制

### 3.1 数据结构
```json
{
  "version": "1.0",
  "servers": [
    {
      "id": "server-1",
      "ip": "192.168.1.100",
      "port": 3478,
      "public_ip": "203.0.113.1",
      "region": "us-east-1",
      "capacity": 1000,
      "load": 0,
      "status": "online",
      "last_heartbeat": "2026-04-05T12:00:00Z"
    },
    {
      "id": "server-2",
      "ip": "192.168.1.101",
      "port": 3478,
      "public_ip": "203.0.113.2",
      "region": "us-west-1",
      "capacity": 1000,
      "load": 0,
      "status": "online",
      "last_heartbeat": "2026-04-05T12:00:00Z"
    }
  ],
  "ttl": 300
}
```

### 3.2 拉取流程
1. ORION服务启动时从HTTPS API拉取服务列表
2. 定期（默认30秒）更新服务列表
3. 缓存服务列表，设置TTL
4. 处理服务列表解析错误和网络异常

## 4. 多机器协作架构

### 4.1 通信协议
- 使用基于TCP的自定义协议
- 消息格式：`[4字节长度][消息类型][消息内容]`
- 支持的消息类型：
  - `PROBE_REQUEST`：探测请求
  - `PROBE_RESPONSE`：探测响应
  - `HEARTBEAT`：心跳
  - `STATUS_UPDATE`：状态更新
  - `ERROR`：错误消息

### 4.2 协调机制
- 主从架构：每个探测请求由一个主服务器协调
- 负载均衡：根据服务列表中的负载信息分配任务
- 故障转移：当某个服务器不可用时，自动切换到其他服务器

### 4.3 状态同步
- 维护服务器状态表
- 定期心跳检测
- 服务健康状态监控

## 5. NAT探测流程

### 5.1 标准STUN探测流程
1. 客户端发送Binding Request到ORION服务器
2. 服务器返回Binding Response，包含客户端的公网地址
3. 客户端分析响应，确定NAT类型

### 5.2 多机器协作探测流程
1. 客户端发送Binding Request到ORION服务器A
2. 服务器A分析请求，确定需要其他服务器参与探测
3. 服务器A通过内部通信协议请求服务器B执行辅助探测
4. 服务器B向客户端发送探测数据包
5. 服务器A收集所有探测结果
6. 服务器A分析结果，确定完整的NAT类型
7. 服务器A向客户端返回Binding Response

### 5.3 NAT类型检测
支持检测以下NAT类型：
- Full Cone NAT
- Restricted Cone NAT
- Port Restricted Cone NAT
- Symmetric NAT

## 6. 数据结构和API接口

### 6.1 核心数据结构
- `OrionServerInfo`：ORION服务器信息
- `ProbeRequest`：探测请求
- `ProbeResponse`：探测响应
- `NATType`：NAT类型枚举
- `ServerStatus`：服务器状态枚举

### 6.2 API接口
- `OrionServer::start()`：启动ORION服务器
- `OrionServer::stop()`：停止ORION服务器
- `OrionServer::handleRequest()`：处理STUN请求
- `ServiceDiscovery::fetchServerList()`：拉取服务器列表
- `Coordinator::coordinateProbe()`：协调探测过程
- `InterServerComm::sendMessage()`：发送机器间消息
- `InterServerComm::receiveMessage()`：接收机器间消息

## 7. 部署和配置方案

### 7.1 服务器配置
- 操作系统：Linux
- CPU：2核或以上
- 内存：4GB或以上
- 网络：公网IP，单网卡
- 端口：UDP 3478（STUN），TCP 3479（内部通信）

### 7.2 网络拓扑
- 所有ORION服务器位于不同的网络环境
- 服务器间通过公网或内网通信
- 客户端通过公网访问ORION服务器

### 7.3 配置文件
```yaml
# orion_server.yaml
server:
  id: "server-1"
  ip: "0.0.0.0"
  port: 3478
  public_ip: "203.0.113.1"
  region: "us-east-1"
  capacity: 1000

service_discovery:
  api_url: "https://api.example.com/orion/servers"
  update_interval: 30
  timeout: 5

inter_server:
  port: 3479
  heartbeat_interval: 10
  timeout: 3

logging:
  level: "info"
  file: "/var/log/orion_server.log"
```

### 7.4 部署步骤
1. 安装依赖
2. 配置服务器
3. 启动服务
4. 验证服务可用性
5. 加入服务列表

## 8. 安全性考虑

### 8.1 认证机制
- 服务器间通信使用TLS加密
- 可选的客户端认证

### 8.2 防攻击措施
- 速率限制
- 流量监控
- 异常检测
- 黑名单机制

### 8.3 数据保护
- 敏感数据加密
- 日志脱敏
- 访问控制

## 9. 扩展性和维护

### 9.1 扩展性
- 水平扩展：添加更多服务器
- 服务发现：自动发现新服务器
- 负载均衡：动态调整负载

### 9.2 监控和维护
- 健康检查
- 性能监控
- 日志分析
- 告警机制

### 9.3 故障处理
- 自动故障检测
- 故障转移
- 恢复机制

## 10. 技术栈

- **语言**：C++11
- **网络**：Boost.Asio
- **序列化**：JSON
- **加密**：OpenSSL
- **配置**：YAML
- **日志**：spdlog
- **构建**：CMake

## 11. 总结

### ORION 全称与寓意
**ORION**：Open Reliable Interconnect ONline，象征精准、可靠的网络连接能力，如同猎户座指引方向，为客户端提供稳定的NAT穿透服务。

本设计方案通过多机器协作实现了完整的ORION服务，解决了单网卡机器无法执行完整NAT探测的问题。系统具有高可用性、可扩展性和安全性，能够满足线上环境的需求。

### 关键优势
- 多机器协作：实现完整的NAT探测
- 服务发现：自动拉取和更新服务列表
- 负载均衡：合理分配探测任务
- 故障转移：提高系统可靠性
- 可扩展性：支持水平扩展

### 后续工作
- 实现核心代码
- 编写测试用例
- 部署到线上环境
- 监控和优化性能

此设计方案已考虑了各种技术细节和边缘情况，为实现多机器协作的ORION服务提供了完整的指导。