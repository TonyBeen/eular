/*************************************************************************
    > File Name: test_variant.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2025年08月26日 星期二 10时04分15秒
 ************************************************************************/

#include <iostream>

#include <variant/variant.h>
#include <variant/type.h>

int main(int argc, char **argv)
{
    rttr::variant var = 10;
    rttr::type type = var.get_type();
    std::cout << type.get_name() << std::endl;

    int &value = var.get_value<int>();
    value = 20;
    std::cout << var.get_value<int>() << std::endl;
    return 0;
}
