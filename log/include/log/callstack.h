/*************************************************************************
    > File Name: callstaack.h
    > Author: hsz
    > Mail:
    > Created Time: Tue 27 Jul 2021 06:02:23 PM CST
 ************************************************************************/

#ifndef __ALIAS_CALLSTACK_H__
#define __ALIAS_CALLSTACK_H__

#include "log_level.h"
#include <stdio.h>
#include <string>
#include <vector>

namespace eular {

class CallStack {
public:
    CallStack();
    CallStack(const char* logtag, int32_t ignoreDepth = 1);
    ~CallStack();

    void clear() { mStackFrame.clear(); }

    // dump the stack of the current call.
    // ignoreDepth: 可忽略的起始调用函数层级；ignoreEnd：可忽略的最后调用函数层级
    void update(uint32_t ignoreDepth = 2, uint32_t ignoreEnd = 0);

    void log(const char* logtag,
             LogLevel::Level level = LogLevel::LEVEL_DEBUG) const;

    // Return a string (possibly very long) containing the complete stack trace.
    std::string toString() const;

    // Get the count of stack frames that are in this call stack.
    size_t size() const { return mStackFrame.size(); }

private:
    std::vector<std::string> mStackFrame;
    uint32_t                 mSkip;
    uint32_t                 mSkipEnd;
};
} // namespace eular

#endif // __ALIAS_CALLSTACK_H__