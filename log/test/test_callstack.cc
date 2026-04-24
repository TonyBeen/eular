/*************************************************************************
    > File Name: test_callstack.cc
    > Author: hsz
    > Brief:
    > Created Time: Thu 07 Dec 2023 08:48:29 AM CST
 ************************************************************************/

#include <stdio.h>
#include <log/log.h>
#include <log/callstack.h>

#define LOG_TAG "test_callstack"

class Foo
{
public:
    Foo()
    {
        eular::CallStack stack;
        stack.update();
        stack.log(LOG_TAG);
    }

    void print()
    {
        printf("%s\n", __PRETTY_FUNCTION__);
        eular::CallStack stack;
        stack.update();
        stack.log(LOG_TAG);
    }

    ~Foo()
    {

    }
};

void create_foo()
{
    Foo f;

    f.print();
}

int main(int argc, char **argv)
{
    create_foo();
    return 0;
}
