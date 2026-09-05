# fcontext

`fcontext` 提供基于 Boost.Context 的底层执行上下文切换能力。它只负责
创建上下文和切换执行流，不包含调度器、线程池或协程生命周期管理。

## 目录结构

```text
include/fcontext/fcontext.h  公共 API
src/asm/                     各平台汇编后端
examples/                    使用示例
test/                        单元测试
```

## 构建

```bash
cmake -S . -B build -DFCONTEXT_BUILD_TESTS=ON -DFCONTEXT_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

运行示例：

```bash
./build/examples/fcontext_example
```

## API 使用

公共头文件为 `fcontext/fcontext.h`，核心接口如下：

```cpp
typedef void* fcontext_t;

typedef struct transfer_t {
    fcontext_t fctx;
    void* data;
} transfer_t;

fcontext_t make_fcontext(void* sp, size_t size, context_entry_t fn);
transfer_t jump_fcontext(fcontext_t to, void* data);
```

完整示例见 [examples/fcontext_example.cc](examples/fcontext_example.cc)。该示例
实现了一个简单的协作式调度器：主线程在两个独立栈上的 worker 之间交替切换，
通过 `transfer_t::data` 投递命令并接收执行事件。基本使用流程是：

1. 分配一块满足对齐要求的栈空间。
2. 使用栈顶地址调用 `make_fcontext`。
3. 使用 `jump_fcontext` 进入新上下文。
4. 保存返回值中的最新 `transfer.fctx`，后续切换必须使用它。
5. 在上下文销毁前保持栈空间有效，入口函数不能直接返回。

`fcontext` 不实现 Linux `getcontext/setcontext` 语义。上下文只能在其保存
栈仍然有效的情况下恢复，不能把 `fctx` 保存下来并在所属栈失效后延迟恢复。
