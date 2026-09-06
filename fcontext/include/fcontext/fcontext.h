#ifndef FCONTEXT_H
#define FCONTEXT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 已挂起执行上下文的不透明句柄。
 */
typedef void* fcontext_t;

/**
 * @brief 上下文切换时传递的数据。
 *
 * 目标上下文收到该结构；当目标上下文再次切回当前上下文时，当前调用
 * 会返回新的 transfer_t。
 */
typedef struct transfer_t {
    fcontext_t fctx;
    void*      data;
} transfer_t;

typedef void (*context_entry_t)(transfer_t transfer);

/**
 * @brief 在指定栈上创建一个新的执行上下文。
 *
 * @param sp 栈顶地址。栈向低地址方向增长。
 * @param size 栈空间大小。
 * @param fn 上下文第一次运行时调用的入口函数。
 * @return 新上下文句柄。
 *
 * 调用者负责分配和释放栈空间。在上下文挂起期间，栈空间必须保持有效，
 * 且不能被其他用途复用。该函数遵循 Boost.Context 的低层接口约定，
 * 参数必须满足前置条件。
 */
fcontext_t make_fcontext(void* sp, size_t size, context_entry_t fn);

/**
 * @brief 挂起当前上下文并切换到目标上下文。
 *
 * @param to 目标上下文句柄。
 * @param data 传递给目标上下文的数据。
 * @return 目标上下文切回当前调用点时返回的上下文和数据。
 *
 * 当前上下文的句柄由返回值中的 fctx 提供。调用者必须保存该句柄，
 * 并在下一次切换时使用最新的 fctx。
 */
transfer_t jump_fcontext(fcontext_t to, void* data);

/**
 * @brief 在目标上下文栈上执行函数后切换上下文。
 *
 * @param to 目标上下文句柄。
 * @param data 传递给目标上下文的数据。
 * @param fn 在目标上下文栈上执行的函数。
 * @return 目标上下文切回当前调用点时返回的上下文和数据。
 */
transfer_t ontop_fcontext(fcontext_t to, void* data, transfer_t (*fn)(transfer_t));

#ifdef __cplusplus
}
#endif

#endif /* FCONTEXT_H */
