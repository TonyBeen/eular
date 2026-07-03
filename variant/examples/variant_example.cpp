/*************************************************************************
    > File Name: test_variant.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2025年08月26日 星期二 10时04分15秒
 ************************************************************************/

#include <iostream>
#include <cstdint>
#include <memory>
#include <string>
#include <functional>

#include <variant/type.h>
#include <variant/variant.h>
#include <variant/detail/type/type_register.h>

enum class VariantEnumTest : uint8_t
{
    Value_0 = 0,
    Value_1 = 1,
    Value_2 = 2
};

struct DemoType
{
    DemoType() = default;
    explicit DemoType(int v) : value(v) {}
    int value = 0;
};

template<typename T>
void print_line(const std::string& title, const T& value)
{
    std::cout << title << value << std::endl;
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    std::cout << "===== variant example =====" << std::endl;

    // 1) 基本构造、有效性与类型信息
    {
        rttr::variant var;
        print_line("empty variant is_valid: ", var.is_valid());

        rttr::type int_type = rttr::type::get<int32_t>();
        print_line("type::get<int32_t>().get_name(): ", int_type.get_name());
        print_line("type::get<int32_t>().is_arithmetic: ", int_type.is_arithmetic());
    }

    // 2) 算术类型：转换、原地转换、快捷接口
    {
        rttr::variant var = 10;
        rttr::type src_type = var.get_type();
        print_line("int variant type: ", src_type.get_name());
        print_line("is_type<int>: ", var.is_type<int>());

        if (var.can_convert<int64_t>())
        {
            int64_t out_value = 0;
            bool ok = var.convert<int64_t>(out_value);
            print_line("convert<int64_t>(out) ok: ", ok);
            print_line("converted int64_t value: ", out_value);
        }

        bool ok_to_string = false;
        std::string text = var.convert<std::string>(&ok_to_string);
        print_line("convert<std::string>(&ok): ", text);
        print_line("convert<std::string> ok: ", ok_to_string);

        // 原地转换为 double
        bool converted = var.convert(rttr::type::get<double>());
        print_line("convert(type::get<double>()) result: ", converted);
        print_line("new type after in-place convert: ", var.get_type().get_name());
        print_line("to_double(): ", var.to_double());
    }

    // 3) 字符串与数字互转、失败路径
    {
        rttr::variant var = std::string("1000");
        print_line("string variant type: ", var.get_type().get_name());
        print_line("can_convert<int64_t>: ", var.can_convert<int64_t>());
        print_line("to_int64(): ", var.to_int64());

        bool ok = false;
        var = std::string("text-xyz");
        int value = var.convert<int>(&ok);
        print_line("convert<int> from text value: ", value);
        print_line("convert<int> from text ok: ", ok);
    }

    // 4) 枚举
    {
        rttr::variant var = VariantEnumTest::Value_1;
        print_line("enum variant type: ", var.get_type().get_name());
        print_line("enum to_int32: ", var.to_int32());

        if (var.is_type<VariantEnumTest>())
        {
            auto& value = var.get_value<VariantEnumTest>();
            value = VariantEnumTest::Value_2;
            print_line("enum after modify: ", static_cast<int32_t>(var.get_value<VariantEnumTest>()));
        }
    }

    // 5) wrapper：reference_wrapper 提取与创建
    {
        int obj = 42;
        rttr::variant wrapped_var = std::ref(obj);

        print_line("wrapped type name: ", wrapped_var.get_type().get_name());
        print_line("wrapped type is_wrapper: ", wrapped_var.get_type().is_wrapper());
        print_line("wrapped underlying type: ", wrapped_var.get_type().get_wrapped_type().get_name());

        if (wrapped_var.can_convert(rttr::type::get<int>()))
        {
            bool ok = false;
            int val = wrapped_var.convert<int>(&ok);
            print_line("convert<int> from reference_wrapper ok: ", ok);
            print_line("convert<int> from reference_wrapper value: ", val);
        }

        const int& wrapped_value = wrapped_var.get_wrapped_value<int>();
        print_line("get_wrapped_value<int>(): ", wrapped_value);

        rttr::variant extracted = wrapped_var.extract_wrapped_value();
        print_line("extract_wrapped_value type: ", extracted.get_type().get_name());
        print_line("extract_wrapped_value value: ", extracted.get_value<int>());

        rttr::variant plain_value = 7;
        bool wrapped_ok = plain_value.convert(rttr::type::get<std::reference_wrapper<int>>());
        print_line("convert(type::get<reference_wrapper<int>>()) ok: ", wrapped_ok);
        if (wrapped_ok)
            print_line("in-place wrapped int: ", plain_value.get_wrapped_value<int>());
    }

    // 6) 指针与 nullptr
    {
        int obj = 100;
        int* p = &obj;
        rttr::variant ptr_var = p;

        print_line("pointer variant type: ", ptr_var.get_type().get_name());
        print_line("can_convert<int*>: ", ptr_var.can_convert<int*>());

        rttr::variant null_var = nullptr;
        print_line("nullptr variant type: ", null_var.get_type().get_name());
        print_line("nullptr is_valid: ", null_var.is_valid());
        print_line("nullptr can_convert<std::nullptr_t>: ", null_var.can_convert<std::nullptr_t>());

        std::nullptr_t np = nullptr;
        bool ok = null_var.convert<std::nullptr_t>(np);
        print_line("convert<std::nullptr_t>(out) ok: ", ok);
        print_line("converted nullptr is null: ", np == nullptr);
    }

    // 7) 自定义类型与按名称查询（含 custom_name）
    {
        rttr::type t = rttr::type::get<DemoType>();
        print_line("DemoType default name: ", t.get_name());
        print_line("DemoType is_class: ", t.is_class());

        // 为演示 get_by_name，显式注册一个自定义名字
        rttr::detail::type_register::custom_name(t, "DemoTypeAlias");

        rttr::type by_name = rttr::type::get_by_name("DemoTypeAlias");
        print_line("get_by_name(\"DemoTypeAlias\") valid: ", by_name.is_valid());
        if (by_name.is_valid())
            print_line("get_by_name(\"DemoTypeAlias\") name: ", by_name.get_name());

        rttr::variant var = DemoType{123};
        print_line("DemoType variant type: ", var.get_type().get_name());
        if (var.is_type<DemoType>())
            print_line("DemoType variant value: ", var.get_value<DemoType>().value);
    }

    // 8) swap / clear
    {
        rttr::variant a = 1;
        rttr::variant b = std::string("hello");

        a.swap(b);
        print_line("after swap a type: ", a.get_type().get_name());
        print_line("after swap b type: ", b.get_type().get_name());

        a.clear();
        print_line("after clear a is_valid: ", a.is_valid());
    }

    // 9) 说明：拥有型 wrapper 的安全限制（当前应为 false）
    {
        int* raw = new int(5);
        rttr::variant var = raw;

        print_line("raw pointer can_convert<std::shared_ptr<int>>: ", var.can_convert<std::shared_ptr<int>>());
        bool ok = false;
        auto ptr = var.convert<std::shared_ptr<int>>(&ok);
        print_line("raw pointer convert<std::shared_ptr<int>> ok: ", ok);
        print_line("converted shared_ptr is null: ", ptr.get() == nullptr);

        // 转换失败时 variant 不接管所有权，调用方自行释放
        delete raw;
    }

    std::cout << "===== done =====" << std::endl;

    return 0;
}
