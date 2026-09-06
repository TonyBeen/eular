#include <cassert>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include <fcontext/fcontext.h>

namespace {

constexpr std::size_t kStackSize = 16u * 1024u;

union ContextStack {
    unsigned char bytes[kStackSize];
    long double   align_long_double;
    void*         align_pointer;
};

enum class EventType {
    Ready,
    Processed,
    Finished,
};

struct Command {
    int         value;
    const char* name;
};

enum class TaskId {
    Producer,
    Consumer,
};

struct Task;

struct Event {
    Task*       task;
    EventType   type;
    const char* name;
    int         value;
    int         total;
};

struct Task {
    TaskId            id;
    const char*       name;
    std::unique_ptr<ContextStack> stack;
    fcontext_t        context;
    const Command*    commands;
    std::size_t       command_count;
    std::size_t       next_command;
    int               total;
    bool              started;
    bool              finished;

    Task(TaskId id, const char* name, const Command* commands,
         std::size_t command_count)
        : id(id),
          name(name),
          stack(new ContextStack()),
          context(nullptr),
          commands(commands),
          command_count(command_count),
          next_command(0),
          total(0),
          started(false),
          finished(false) {}

    ~Task()
    {
        // 已启动的上下文只能在 Finished 后销毁，否则 context 仍可能被恢复。
        assert(!started || finished);
    }
};

void task_entry(transfer_t transfer)
{
    /*
    * 第一次进入 task_entry 时，data 是主调度器传入的 Task 指针。
    * task_entry 保存在独立的 task 栈上，因此 self 和下面的局部变量
    * 会在每次上下文恢复后继续保持。
    */
    Task* self = static_cast<Task*>(transfer.data);

    // 这个作用域结束后，主体逻辑中的局部对象会在最终切回调度器前析构。
    {
        Event ready = {self, EventType::Ready, "ready", 0, self->total};

        /*
        * 当前 task 挂起，切回主调度器。
        *
        * transfer.fctx 是主调度器调用 jump_fcontext() 时保存的恢复点，
        * 不是一个永久不变的主协程句柄。当前 task 的恢复点会通过这次
        * jump_fcontext() 的返回值交给主调度器。
        */
        transfer = jump_fcontext(transfer.fctx, &ready);

        /*
        * 只有主调度器下一次恢复当前 task 时，才会从上一行继续执行。
        * 此时 transfer.fctx 是主调度器本次调用 jump_fcontext() 保存的
        * 新恢复点；调度器传入的 data 可以为空，任务状态保存在 self 中。
        */

        while (self->next_command < self->command_count) {
            const Command& command = self->commands[self->next_command++];
            self->total += command.value;
            Event processed = {self, EventType::Processed, command.name, command.value, self->total};

            /* 当前 task 挂起，把 processed 事件交给主调度器。 */
            transfer = jump_fcontext(transfer.fctx, &processed);

            /* 主调度器恢复当前 task 后，从这里继续执行。 */
        }
    }

    Event finished = {self, EventType::Finished, "finished", 0, self->total};

    /*
     * 把完成事件交给主调度器。主调度器收到 Finished 后不会再次恢复
     * 当前 task，因此正常执行路径不会回到这条 jump_fcontext() 之后。
     */
    (void)jump_fcontext(transfer.fctx, &finished);
}

Event run_once(Task& task)
{
    transfer_t transfer;
    if (!task.started) {
        /* 第一次调度：在 task 自己的栈上建立入口上下文。 */
        task.context = make_fcontext(task.stack->bytes + sizeof(task.stack->bytes),
                                     sizeof(task.stack->bytes),
                                     task_entry);
        task.started = true;

        /*
         * 当前运行者是主调度器。jump_fcontext() 会保存主调度器恢复点，
         * 然后切换到新 task；task_entry 收到的 transfer.fctx 就是这个
         * 主调度器恢复点。
         */
        transfer = jump_fcontext(task.context, &task);
    } else {
        /* 后续调度：恢复 task 上一次主动挂起时保存的恢复点。 */
        transfer = jump_fcontext(task.context, nullptr);
    }

    /*
     * task 已经切回主调度器。返回值中的 fctx 是 task 刚刚保存的恢复点，
     * 必须保存起来，供下一次调度恢复该 task。
     */
    task.context = transfer.fctx;
    Event event = *static_cast<Event*>(transfer.data);
    if (event.type == EventType::Finished) task.finished = true;
    return event;
}

void print_event(const Event& event)
{
    std::cout << event.task->name << ": " << event.name;
    if (event.type == EventType::Processed) std::cout << " value=" << event.value;
    std::cout << " total=" << event.total << '\n';
}

void destroy_task(std::vector<std::unique_ptr<Task> >& tasks, Task* task)
{
    for (std::vector<std::unique_ptr<Task> >::iterator it = tasks.begin();
         it != tasks.end(); ++it) {
        if (it->get() == task) {
            task->context = nullptr;
            tasks.erase(it);
            return;
        }
    }
}

struct ScheduleResult {
    int producer_total;
    int consumer_total;
};

ScheduleResult schedule(std::vector<std::unique_ptr<Task> >& tasks,
                        std::deque<Task*>& ready_queue)
{
    ScheduleResult result = {0, 0};

    while (!ready_queue.empty()) {
        Task* task = ready_queue.front();
        ready_queue.pop_front();

        Event event = run_once(*task);
        print_event(event);
        if (!task->finished) {
            ready_queue.push_back(task);
            continue;
        }

        if (task->id == TaskId::Producer) {
            result.producer_total = event.total;
        } else {
            result.consumer_total = event.total;
        }

        // 此处已经不会再次恢复 task，销毁 Task 同时释放其上下文栈。
        destroy_task(tasks, task);
    }

    return result;
}

}  // 匿名命名空间

int main()
{
    const Command producer_commands[] = {
        {3, "produce"},
        {5, "produce"},
    };
    const Command consumer_commands[] = {
        {2, "consume"},
        {4, "consume"},
    };

    std::vector<std::unique_ptr<Task> > tasks;
    std::unique_ptr<Task> producer(new Task(TaskId::Producer, "producer",
                                            producer_commands, 2));
    std::unique_ptr<Task> consumer(new Task(TaskId::Consumer, "consumer",
                                            consumer_commands, 2));

    Task* producer_ptr = producer.get();
    Task* consumer_ptr = consumer.get();
    tasks.push_back(std::move(producer));
    tasks.push_back(std::move(consumer));

    std::deque<Task*> ready_queue;
    ready_queue.push_back(producer_ptr);
    ready_queue.push_back(consumer_ptr);
    const ScheduleResult result = schedule(tasks, ready_queue);

    printf("producer total=%d, consumer total=%d\n", result.producer_total, result.consumer_total);
    return 0;
}
