#include <fcontext/fcontext.h>

#include "catch/catch.hpp"

namespace {

constexpr size_t kStackSize = 64u * 1024u;

union ContextStack {
    unsigned char bytes[kStackSize];
    long double   align_long_double;
    void*         align_pointer;
};

fcontext_t     fiber_context;
volatile int   fiber_step;
volatile void* fiber_data;

void fiber_entry(transfer_t transfer)
{
    fiber_data = transfer.data;
    fiber_step = 1;

    transfer = jump_fcontext(transfer.fctx, reinterpret_cast<void*>(2));
    fiber_data = transfer.data;
    fiber_step = 2;

    transfer = jump_fcontext(transfer.fctx, reinterpret_cast<void*>(3));
    fiber_data = transfer.data;
    fiber_step = 3;

    /* 入口函数不能返回，汇编后端的终止路径会结束进程。 */
    (void)jump_fcontext(transfer.fctx, reinterpret_cast<void*>(4));
}

struct LifetimeProbe {
    int* destroyed;

    explicit LifetimeProbe(int* destroyed) : destroyed(destroyed) {}

    ~LifetimeProbe()
    {
        ++*destroyed;
    }
};

void cleanup_entry(transfer_t transfer)
{
    int* destroyed = static_cast<int*>(transfer.data);

    {
        LifetimeProbe probe(destroyed);
        (void)jump_fcontext(transfer.fctx, nullptr);
    }

    // 作用域已结束，probe 已析构，再通知主上下文任务完成。
    (void)jump_fcontext(transfer.fctx, reinterpret_cast<void*>(1));
}

}  // 匿名命名空间

TEST_CASE("fcontext switches and returns transfer data", "[fcontext]")
{
    ContextStack stack = {};
    fiber_step = 0;
    fiber_data = nullptr;
    fiber_context = make_fcontext(stack.bytes + sizeof(stack.bytes), sizeof(stack.bytes), fiber_entry);
    REQUIRE(fiber_context != nullptr);

    transfer_t transfer = jump_fcontext(fiber_context, reinterpret_cast<void*>(1));
    fiber_context = transfer.fctx;
    CHECK(fiber_step == 1);
    CHECK(fiber_data == reinterpret_cast<void*>(1));

    transfer = jump_fcontext(fiber_context, reinterpret_cast<void*>(10));
    fiber_context = transfer.fctx;
    CHECK(fiber_step == 2);
    CHECK(fiber_data == reinterpret_cast<void*>(10));

    transfer = jump_fcontext(fiber_context, reinterpret_cast<void*>(20));
    CHECK(fiber_step == 3);
    CHECK(fiber_data == reinterpret_cast<void*>(20));
}

TEST_CASE("fiber locals are cleaned up before final switch", "[fcontext]")
{
    ContextStack stack = {};
    int destroyed = 0;
    fcontext_t context = make_fcontext(stack.bytes + sizeof(stack.bytes),
                                       sizeof(stack.bytes), cleanup_entry);

    transfer_t transfer = jump_fcontext(context, &destroyed);
    context = transfer.fctx;
    CHECK(destroyed == 0);

    (void)jump_fcontext(context, nullptr);
    CHECK(destroyed == 1);
}
