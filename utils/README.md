# utils

`utils` 是一组 C/C++ 基础工具库，提供字符串、容器、缓存、线程同步、文件目录、时间、序列化、编码转换等常用能力。

项目使用 CMake 构建，默认同时生成静态库和动态库，公共头文件位于 `include/utils/`。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

启用测试：

```bash
cmake -S . -B build -DUTILS_ENABLE_TEST=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

常用选项：

| 选项 | 默认值 | 说明 |
| --- | --- | --- |
| `UTILS_STATIC_LIB` | `ON` | 构建静态库 `utils_static` |
| `UTILS_DYNAMIC_LIB` | `ON` | 构建动态库 `utils` |
| `UTILS_ENABLE_TEST` | `OFF` | 构建测试用例 |
| `UTILS_ENABLE_ASAN` | `OFF` | Linux 下启用 ASan/LSan/UBSan |
| `UTILS_ENABLE_DEBUG` | `OFF` | 使用 Debug 构建类型 |
| `UTILS_BIMAP_USE_STD_CONTAINER` | `OFF` | `BiMap` 使用标准库容器作为后端 |
| `UTILS_SUBMODULE_ENABLE_INSTALL` | `ON` | 作为子模块时安装头文件和库 |

预设构建：

```bash
cmake --preset default
cmake --build --preset default

cmake --preset musl
cmake --build --preset musl
```

## CMake 使用

安装后可通过包配置引入：

```cmake
find_package(utils CONFIG REQUIRED)

target_link_libraries(your_target PRIVATE eular::utils_static)
# 或
target_link_libraries(your_target PRIVATE eular::utils)
```

未安装时，也可以作为子目录使用：

```cmake
add_subdirectory(path/to/utils)
target_link_libraries(your_target PRIVATE eular::utils_static)
```

## 主要模块

### 容器与数据结构

| 头文件 | 说明 |
| --- | --- |
| `map.h` | 基于红黑树的 `Map`，支持 COW、`emplace`、`contains`、范围 erase、merge 等接口 |
| `map_node.h` | `Map` 的红黑树节点和底层存储实现 |
| `lru_cache.hpp` | LRU 缓存，支持容量淘汰、删除回调、无拷贝 key 查询 |
| `bimap.h` / `bimap.hpp` | 双向映射容器 |
| `hash.h` | 哈希表实现 |
| `consistent_hash.h` | 一致性哈希 |
| `ring_buffer.h` | 环形缓冲区 |
| `buffer.h` / `shared_buffer.h` | 二进制缓冲区和共享缓冲区 |
| `bitmap.h` | 位图工具 |

### 字符串、序列化与类型工具

| 头文件 | 说明 |
| --- | --- |
| `string8.h` | 字符串类，支持格式化、追加、比较和转换 |
| `string_view.hpp` | `string_view` 兼容实现 |
| `serialize.hpp` | 基础序列化工具 |
| `any.hpp` | `any` 类型容器 |
| `optional.hpp` | `optional` 类型容器 |
| `enum_util.hpp` | 枚举工具 |
| `types.hpp` | 基础类型定义 |
| `literals.hpp` | 字面量辅助工具 |
| `has_member.hpp` | 成员检测工具 |

### 线程与同步

| 头文件 | 说明 |
| --- | --- |
| `thread.h` | 线程封装，支持继承 `ThreadBase` 或使用 `Thread` 绑定工作函数 |
| `thread_local.h` | 线程局部存储 |
| `mutex.h` | `pthread_mutex_t` 封装，提供 `AutoLock` |
| `condition.h` | 条件变量封装，支持 wait/signal/broadcast/timedWait |
| `semaphore.h` | 信号量封装 |

### 文件、时间与系统工具

| 头文件 | 说明 |
| --- | --- |
| `file.h` | 文件操作 |
| `dir.h` | 目录操作 |
| `time.h` | 时间工具 |
| `timer.h` | 定时器 |
| `elapsed_time.h` | 耗时统计 |
| `uuid.h` | UUID 生成 |
| `platform.h` / `sysdef.h` | 平台、编译器、架构宏定义 |
| `errors.h` / `exception.h` | 错误码和异常类型 |
| `debug.h` | 日志调试宏 |

### 编码、字节序与流

| 头文件 | 说明 |
| --- | --- |
| `code_convert.h` | 编码转换 |
| `endian.hpp` | 字节序转换 |
| `buffer_stream.h` / `buffer_stream_utils.h` | 缓冲区流工具 |
| `alloc.h` | 内存分配辅助函数 |
| `auto_clean.hpp` | 自动清理工具 |
| `singleton.h` / `singleton_object.h` | 单例辅助模板 |
| `utils.h` | 通用宏和工具定义 |

## 容器说明

### `eular::Map`

`Map` 是基于红黑树实现的有序映射容器。当前支持：

- 插入：`insert`、`emplace`
- 查询：`find`、`contains`、`value`、`operator[]`
- 删除：按 key、按 iterator、按 iterator 范围
- 遍历：`begin/end`、`cbegin/cend`、`rbegin/rend`、`crbegin/crend`
- 其它：`size`、`empty`、`clear`、`swap`、`merge`

注意：

- 非 const 迭代器接口会触发 COW detach。
- `emplace(key, value)` 是 key/value 两参接口，重复 key 时不构造 value，语义更接近简化版 `try_emplace`。
- 不提供可变 `rbegin/rend`，避免反向遍历中 `erase(iterator)` 的返回方向语义不一致。

### `eular::LruCache`

`LruCache` 提供固定容量缓存，容量满时淘汰最旧条目。容量传 `0` 表示不限制。

```cpp
eular::LruCache<int, const char*> cache(2);

cache.put(1, "one");
cache.put(2, "two");
cache.get(1);          // 访问 1，使其变为较新条目
cache.put(3, "three"); // 淘汰 key 2
```

行为说明：

- `get()` 命中后会刷新 LRU 顺序。
- `put()` 遇到重复 key 返回 `false`，不会替换已有值，也不会触发容量淘汰。
- `OnEntryRemoved` 回调中的异常会被捕获，不影响缓存内部状态清理。
- 当前 `Iterator` 遍历的是内部 hash set 顺序，不承诺 LRU 顺序。

## 测试

测试基于 Catch2，位于 `test/`。

只运行某个测试：

```bash
cmake --build build --target test_map
./build/test/test_map

cmake --build build --target test_lru_cache
./build/test/test_lru_cache
```

运行全部测试：

```bash
ctest --test-dir build --output-on-failure
```
