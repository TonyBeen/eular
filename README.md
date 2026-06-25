# eular 工程总览

本文档为仓库总入口，汇总所有主模块的定位、依赖、构建开关与文档入口。

## 1. 主模块清单

- config: 通用配置解析与观察者（yaml/json/xml/ini）
- crypto: 加密与摘要能力封装
- fmt: 第三方格式化头文件集合
- kcp: 基于 UDP 的可靠传输协议实现
- libevent: libevent 封装与 DNS 异步能力
- libutp: UTP 可靠传输库（含拥塞控制、会话恢复等）
- log: 日志库与调用栈
- ntrs: 私有 NAT 穿透项目（与 stun 并存）
- reflection: C++11 学习向最小反射实现
- sqlutils: MySQL/Redis/SQLite 封装（Makefile 工程）
- stun: STUN 与 NAT 探测、控制面节点
- utils: 通用工具底座
- variant: 运行时类型和值容器头文件库

## 2. 全局依赖矩阵

- utils:
  - Threads（CMake: find_package(Threads REQUIRED) + Threads::Threads）
- log:
  - Threads（CMake: find_package(Threads REQUIRED) + Threads::Threads）
  - libunwind（可选，启用 callstack）
- crypto:
  - OpenSSL/GnuTLS/MbedTLS/Windows Crypto（按开关）
- config:
  - tinyxml2
  - nlohmann-json
  - libyaml
  - yyjson
  - c4core
  - Threads
- libevent:
  - libevent_v2
  - c-ares（可选）
- kcp:
  - libevent（兄弟工程）
  - utils（兄弟工程）
  - ntrs（可选桥接）
- libutp:
  - utils（兄弟工程）
  - libevent（兄弟工程）
  - boringssl（子模块）
  - dl（Linux）
- stun:
  - utils（兄弟工程）
  - libevent（兄弟工程）
  - mosquitto（3rd_party）
  - OpenSSL + zlib（启用 TLS 时）
- ntrs:
  - utils（兄弟工程）
  - libevent（兄弟工程）
- reflection:
  - 仅 C++11 标准库
- variant:
  - 头文件库，无额外链接依赖

## 3. 各模块详细说明

### 3.1 config

模块目录: config

功能:

- 统一配置解析接口，覆盖 yaml/json/xml/ini
- 提供配置观察者 ConfigObserver，用于配置更新后路径匹配通知

构建开关:

- CONFIG_BUILD_TESTS
- CONFIG_BUILD_EXAMPLES
- CONFIG_BUILD_SHARED
- CONFIG_BUILD_STATIC

典型构建:

    cmake -S config -B config/build -DCONFIG_BUILD_EXAMPLES=ON
    cmake --build config/build

文档入口:

- config/README

---

### 3.2 crypto

模块目录: crypto

功能:

- AES、RSA、MD5、SHA、Base64、CRC32
- 后端按平台和选项切换（OpenSSL/GnuTLS/MbedTLS/Windows Crypto）

构建开关:

- CRYPTOPP_STATIS_LIB
- CRYPTOPP_DYNAMIC_LIB
- ENABLE_OPENSSL
- ENABLE_GNUTLS
- ENABLE_MBEDTLS
- ENABLE_WINDOWS_CRYPTO

典型构建:

    cmake -S crypto -B crypto/build -DCRYPTOPP_STATIS_LIB=ON
    cmake --build crypto/build

文档入口:

- include 说明集中在 crypto 目录与 test 示例

---

### 3.3 fmt

模块目录: fmt

功能:

- 提供格式化头文件集合（fmt.h、format.h、chrono.h 等）

说明:

- 当前仓库中 fmt 以头文件形式存在，未见独立 CMake 构建入口。
- 使用时按包含路径直接 include。

---

### 3.4 kcp

模块目录: kcp

功能:

- UDP 可靠传输
- 示例和测试工具
- 可选 NTRS bridge 集成

关键开关:

- KCPP_DYNAMIC_LIB
- KCPP_STATIC_LIB
- BUILD_EXAMPLES
- BUILD_TEST_TOOLS
- KCPP_ENABLE_NTRS
- USE_SENDMMSG

依赖路径可配置:

- KCPP_EVENT_ROOT
- KCPP_UTILS_ROOT
- KCPP_NTRS_ROOT

---

### 3.5 libevent

模块目录: libevent

功能:

- 对 libevent 的工程化封装
- 统一异步 loop/timer/poll 接口
- 可选 c-ares DNS backend

关键开关:

- EVENT_DYNAMIC_LIB
- EVENT_STATIC_LIB
- EVENT_USE_CARES
- EVENT_WRAPPER_ENABLE_PRECISE_TIMER

说明:

- 上层项目多以 add_subdirectory 方式复用该模块。

---

### 3.6 libutp

模块目录: libutp

功能:

- UTP 可靠传输
- 多流、拥塞控制、MTU 探测、会话恢复、0-RTT

关键开关:

- UTP_BUILD_TESTS
- UTP_BUILD_LSQUIC_ECHO
- UTP_USE_BUNDLED_BORINGSSL
- UTP_ENABLE_FAULT_INJECTION
- UTP_USE_SENDMMSG

依赖路径可配置:

- UTP_UTILS_ROOT
- UTP_EVENT_ROOT

文档入口:

- libutp/README

---

### 3.7 log

模块目录: log

功能:

- 日志级别、格式化、写入策略
- callstack（依赖 libunwind 时启用）

关键开关:

- LOG_BUILD_STATIC
- LOG_BUILD_TESTLOG
- LOG_BUILD_LOGCAT
- LOG_BUILD_CALLSTACK_TEST
- LOG_BUILD_BENCHMARK

文档入口:

- log/README

---

### 3.8 ntrs

模块目录: ntrs

功能:

- 私有 NAT 穿透项目
- 与 stun 并存的独立代码树

关键开关:

- NTRS_BUILD_EXAMPLES
- BUILD_SHARED_LIBS

依赖路径可配置:

- NTRS_EVENT_ROOT
- NTRS_UTILS_ROOT

文档入口:

- ntrs/README.md

---

### 3.9 reflection

模块目录: reflection

功能:

- C++11 最小运行时反射
- 类型注册、属性访问、方法重载匹配
- 构造与析构统一入口

当前实现特点:

- 自研 Type/Value（不依赖外部 variant）
- invoke_best 采用参数匹配分数决议
- 使用 make_index_sequence 展开构造与方法参数

关键开关:

- REFLECTION_BUILD_EXAMPLES
- REFLECTION_BUILD_TESTS

文档入口:

- reflection/README.md

---

### 3.10 sqlutils

模块目录: sqlutils

功能:

- mysql/redis/sqlite 封装
- 以 Makefile 工程组织

目录要点:

- mysql.cpp/mysql.h
- redis.cpp/redis.h
- sqlite.cpp/sqlite.h
- test_mysql.cc
- test_redis.cc

说明:

- 当前未检测到独立 README，建议后续补充构建与链接说明。

---

### 3.11 stun

模块目录: stun

功能:

- STUN NAT 探测
- 控制面节点与 hub 协同
- 示例、测试与部署文档

关键开关:

- BUILD_EXAMPLES
- STUN_STATIC_EXECUTABLES
- STUN_MQTT_WITH_TLS
- STUN_MUSL_STATIC

依赖路径可配置:

- STUN_EVENT_ROOT
- STUN_UTILS_ROOT
- STUN_MUSL_ROOT

文档入口:

- stun/doc/README.md

---

### 3.12 utils

模块目录: utils

功能:

- 线程、锁、条件变量
- 字符串与缓冲
- 容器与内存工具

关键开关:

- UTILS_STATIC_LIB
- UTILS_DYNAMIC_LIB
- UTILS_ENABLE_TEST
- UTILS_ENABLE_ASAN
- UTILS_BIMAP_USE_STD_CONTAINER

文档入口:

- utils/README.md

---

### 3.13 variant

模块目录: variant

功能:

- rttr::variant 与 rttr::type 头文件实现
- 运行时类型查询、值封装、转换能力

关键开关:

- VARIANT_BUILD_TEST
- VARIANT_BUILD_EXAMPLES

说明:

- CMake 以 INTERFACE 库导出，主要为头文件安装。

文档入口:

- variant/README

## 4. 构建建议

仓库采用多模块并行开发模式，建议按模块单独构建，不建议一次性全仓库统一编译。

推荐流程:

1. 先确定目标模块。
2. 阅读该模块 README 与 CMake 选项。
3. 指定依赖根路径（如 event/utils/ntrs）。
4. 模块内单独配置和构建。

## 5. 文档维护约定

为避免新增代码后文档落后，统一采用以下约定：

1. 模块新增入口文件时，必须更新本 README 的对应模块章节。
2. 模块新增示例或测试时，补充到模块说明中的构建与验证信息。
3. 新增依赖或切换依赖后端时，必须更新依赖矩阵。
4. 根 README 负责总览，模块细节放在各自目录 README。
5. 3rd_party 内容默认不在根 README 展开，只在模块 README 中按需说明。
