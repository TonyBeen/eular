/*************************************************************************
    > File Name: test_utils.cc
    > Author: hsz
    > Brief:
    > Created Time: Mon 10 Jan 2022 09:49:33 AM CST
 ************************************************************************/

#include <utils/utils.h>
#include <iostream>
#include <assert.h>

using namespace std;

int main(int argc, char **argv)
{
    std::list<std::string> fileList;
    auto ret = ForeachDir(".", fileList);
    assert(ret == fileList.size());
    for (const auto &tmp : fileList) {
        printf("%s\n", tmp.c_str());
    }

    return 0;
}
