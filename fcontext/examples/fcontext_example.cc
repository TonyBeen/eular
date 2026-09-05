#include <cstddef>
#include <cstdlib>
#include <iostream>

#include <fcontext/fcontext.h>

namespace {

constexpr std::size_t kStackSize = 64u * 1024u;

union ContextStack {
    unsigned char bytes[kStackSize];
    long double   align_long_double;
    void*         align_pointer;
};

struct Fiber;

struct Command {
    int         value;
    const char* name;
    bool        stop;
};

struct Event {
    Fiber*      fiber;
    const char* name;
    int         value;
    int         total;
};

struct Fiber {
    const char* name;
    ContextStack stack;
    fcontext_t   context;
    int          total;
};

void fiber_entry(transfer_t transfer)
{
    /* 入口数据只用于取得 Fiber 对象；之后该局部指针会保存在协程栈上。 */
    Fiber* self = static_cast<Fiber*>(transfer.data);

    Event ready = {self, "ready", 0, self->total};
    transfer = jump_fcontext(transfer.fctx, &ready);

    for (;;) {
        const Command* command = static_cast<const Command*>(transfer.data);
        if (command->stop) {
            Event stopped = {self, "stopped", 0, self->total};
            (void)jump_fcontext(transfer.fctx, &stopped);
            std::abort();
        }

        self->total += command->value;
        Event processed = {self, command->name, command->value, self->total};
        transfer = jump_fcontext(transfer.fctx, &processed);
    }
}

Event start_fiber(Fiber& fiber)
{
    fiber.context = make_fcontext(fiber.stack.bytes + sizeof(fiber.stack.bytes),
                                   sizeof(fiber.stack.bytes),
                                   fiber_entry);

    transfer_t transfer = jump_fcontext(fiber.context, &fiber);
    fiber.context = transfer.fctx;
    return *static_cast<Event*>(transfer.data);
}

Event resume_fiber(Fiber& fiber, Command& command)
{
    transfer_t transfer = jump_fcontext(fiber.context, &command);
    fiber.context = transfer.fctx;
    return *static_cast<Event*>(transfer.data);
}

void print_event(const Event& event)
{
    std::cout << event.fiber->name << ": " << event.name;
    if (event.value != 0) std::cout << " value=" << event.value;
    std::cout << " total=" << event.total << '\n';
}

}  // 匿名命名空间

int main()
{
    Fiber producer = {"producer", {}, nullptr, 0};
    Fiber consumer = {"consumer", {}, nullptr, 0};

    print_event(start_fiber(producer));
    print_event(start_fiber(consumer));

    Command producer_commands[] = {
        {3, "produce", false},
        {5, "produce", false},
    };
    Command consumer_commands[] = {
        {2, "consume", false},
        {4, "consume", false},
    };

    print_event(resume_fiber(producer, producer_commands[0]));
    print_event(resume_fiber(consumer, consumer_commands[0]));
    print_event(resume_fiber(producer, producer_commands[1]));
    print_event(resume_fiber(consumer, consumer_commands[1]));

    Command stop = {0, "stop", true};
    print_event(resume_fiber(producer, stop));
    print_event(resume_fiber(consumer, stop));
}
