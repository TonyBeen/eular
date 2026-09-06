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
实现了一个简单的 M:1 协作式调度器：多个用户态 fiber 共用一个主线程，主线程
通过 ready queue 在两个独立栈上的 worker 之间轮转，并通过 `transfer_t::data`
接收执行事件。示例中的 `Task` 是堆对象，独占自己的上下文栈；调度器是任务的
所有者，并在收到 `Finished` 后立即回收任务和栈。基本使用流程是：

1. 分配一块满足对齐要求的栈空间。
2. 使用栈顶地址调用 `make_fcontext`。
3. 使用 `jump_fcontext` 进入新上下文。
4. 保存返回值中的最新 `transfer.fctx`，后续切换必须使用它。
5. 在上下文销毁前保持栈空间有效；入口函数完成后先切回调度器，调度器
   确认收到完成事件后再销毁任务和栈。

## 生命周期约束

`make_fcontext()` 只初始化调用者提供的栈，不负责分配或释放内存。因此工程代码
通常让任务对象通过 RAII 持有堆上的栈，并保证以下关系：

```text
Task 存活
  └── ContextStack 存活
        └── fcontext_t 可以被恢复
```

任务处于挂起状态时不能销毁 `Task`。只有 fiber 已经切回调度器并报告完成，且调度器
承诺不再使用该 `fcontext_t`，才可以释放栈。

`jump_fcontext()` 不会像普通函数调用那样自动展开当前栈帧。需要在入口函数最终
切回调度器之前，用内层作用域结束需要清理的局部对象：

```cpp
void task_entry(transfer_t transfer)
{
    Task* task = static_cast<Task*>(transfer.data);
    {
        Resource resource = acquire_resource();
        Event yield_event = make_yield_event(task);
        // 可以在这个作用域内多次 jump_fcontext() 挂起和恢复。
        transfer = jump_fcontext(transfer.fctx, &yield_event);
    }  // resource 在这里析构

    // 发送 Finished 后，调度器才可以销毁 Task。
    Event finished_event = make_finished_event(task);
    (void)jump_fcontext(transfer.fctx, &finished_event);
}
```

入口函数不能直接 `return`。它应在完成后切回调度器，并保证调度器不会再次恢复
该上下文；否则汇编后端的终止路径或已释放的栈可能被执行。

`fcontext` 不实现 Linux `getcontext/setcontext` 语义。上下文只能在其保存
栈仍然有效的情况下恢复，不能把 `fctx` 保存下来并在所属栈失效后延迟恢复。
