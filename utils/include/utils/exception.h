/*************************************************************************
    > File Name: exception.h
    > Author: hsz
    > Mail:
    > Created Time: Wed 29 Sep 2021 08:52:55 AM CST
 ************************************************************************/

#ifndef __EXCEPTION_H__
#define __EXCEPTION_H__

#include <exception>

#include <utils/sysdef.h>
#include <utils/string8.h>

namespace eular {

class Exception : public std::exception
{
public:
    Exception(const char *msg) : mExceptMsg(msg) {}
    Exception(const String8 &msg) : mExceptMsg(msg) {}
    ~Exception() override= default;

    virtual const char *what() const noexcept
    {
        return mExceptMsg.c_str();
    }
private:
    String8     mExceptMsg;
};

class bad_type_cast_exception : public std::exception
{
public:
    bad_type_cast_exception(const char *msg) : mMessage(msg) {}
    bad_type_cast_exception(const String8& msg) : mMessage(msg) {}
    ~bad_type_cast_exception() override = default;

    virtual const char *what() const noexcept
    {
        return mMessage.c_str();
    }

private:
    String8 mMessage;
};

    
} // namespace eular

#endif // __EXCEPTION_H__