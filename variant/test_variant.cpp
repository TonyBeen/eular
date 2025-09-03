/*************************************************************************
    > File Name: test_variant.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2025年08月26日 星期二 10时04分15秒
 ************************************************************************/

#include <iostream>

#include <variant/variant.h>
#include <variant/type.h>

#include "test_enums.h"

int main(int argc, char **argv)
{
    {
        rttr::variant var;
        rttr::type type = rttr::type::get<int32_t>();
    }
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

    {
        rttr::variant var = variant_enum_test::VALUE_1;
        rttr::type type = var.get_type();
        std::cout << type.get_name() << std::endl;
        if (var.can_convert<variant_enum_test>())
        {
            variant_enum_test &value = var.get_value<variant_enum_test>();
            value = variant_enum_test::VALUE_2;
            std::cout << (int32_t)var.get_value<variant_enum_test>() << std::endl;
        }
    }

    {
        int obj = 42;
        rttr::variant var = std::ref(obj);

        std::cout << var.can_convert(rttr::type::get<int>()) << std::endl;

        bool ok = false;
        int val = var.convert<int>(&ok);
        std::cout << ok << std::endl;
        std::cout << val << std::endl;
        // CHECK(ok == true);
        // CHECK(val == obj);

        // CHECK(var.convert(type::get<int>()) == true);

        // int val_2;
        // CHECK(var.convert<int>(val_2) == true);
        // CHECK(val_2 == obj);
    }

    return 0;
}
