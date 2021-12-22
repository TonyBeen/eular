/*************************************************************************
    > File Name: callstack.cpp
    > Author: hsz
    > Mail:
    > Created Time: Tue 27 Jul 2021 06:02:27 PM CST
 ************************************************************************/

#define UNW_LOCAL_ONLY
#include "callstack.h"
#include <cxxabi.h>
#include <libunwind/libunwind.h>
#include <stdlib.h>

namespace eular {
CallStack::CallStack() :
    mSkip(2),
    mSkipEnd(0)
{

}

CallStack::CallStack(const char* logtag, int32_t ignoreDepth)
{
    this->update(ignoreDepth + 1);
    this->log(logtag);
}

CallStack::~CallStack()
{
    
}

void CallStack::update(uint32_t ignoreDepth, uint32_t ignoreEnd)
{
    mSkip = ignoreDepth;
    mSkipEnd = ignoreEnd;
    unw_cursor_t cursor;
    unw_context_t context;

    mStackFrame.clear();
    unw_getcontext(&context);
    unw_init_local(&cursor, &context);

    while (unw_step(&cursor) > 0)
    {
        unw_word_t offset, funcPointer;
        unw_get_reg(&cursor, UNW_REG_IP, &funcPointer);
        if (funcPointer == 0) {
            break;
        }

        char sym[256] = {0};
        if (unw_get_proc_name(&cursor, sym, sizeof(sym), &offset) == 0) {
            char *nameptr = sym;
            int status = -1;
            char *demangled = abi::__cxa_demangle(sym, nullptr, nullptr, &status);
            if (status == 0 && demangled != nullptr) {
                nameptr = demangled;
            }

            // char buf[256] = {0};
            // sprintf(buf, "-0x%012lx: (%s + 0x%lx)", funcPointer, nameptr, offset);
            String8 str = String8::format("-0x%016lx: (%s + 0x%lx)", funcPointer, nameptr, offset);
            mStackFrame.push_back(str);
            if (demangled) {
                free(demangled);
            }
        } else {
            mStackFrame.push_back(String8::format("(unable to obtain symbol name for this frame)"));
        }
    }
}

void CallStack::log(const char* logtag, LogLevel::Level level) const
{
    for (size_t i = mSkipEnd; i < mStackFrame.size() - mSkip; ++i) {
        log_write(level, logtag, "%s\n", mStackFrame[i].c_str());
    }
}

String8 CallStack::toString() const
{
    String8 str;
    for (size_t i = 0; i < mStackFrame.size(); ++i) {
        str += mStackFrame[i];
        str += "\n";
    }
    return std::move(str);
}

} // namespace eular