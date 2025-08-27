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
    // 测试int
    {
        rttr::variant var = 10;
        rttr::type type = var.get_type();
        std::cout << type.get_name() << std::endl;

        if (var.can_convert<int64_t>())
        {
            int64_t &value = var.get_value<int64_t>();
            value = 20;
            std::cout << var.get_value<int>() << std::endl;
        }
    }

    // 测试 float
    {
        rttr::variant var = 10.0f;
        rttr::type type = var.get_type();
        std::cout << type.get_name() << std::endl;

        if (var.can_convert<double>())
        {
            double &value = var.get_value<double>();
            value = 20.0;
            std::cout << var.get_value<double>() << std::endl;
        }
    }

    // 测试string
    {
        rttr::variant var = "hello";
        rttr::type type = var.get_type();
        std::cout << type.get_name() << std::endl;

        if (var.can_convert<std::string>())
        {
            std::string &value = var.get_value<std::string>();
            value = "world";
            std::cout << var.get_value<std::string>() << std::endl;
        }

        var = "1000";
        if (var.can_convert<int64_t>())
        {
            std::cout << var.convert<int64_t>() << std::endl;
        }
    }

    return 0;
}
