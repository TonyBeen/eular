#include "reflection/mini_reflect.h"

#include <iostream>
#include <string>
#include <vector>

struct Calculator {
    Calculator() : base(10) {}
    explicit Calculator(int b) : base(b) {}

    int sum(int x) const { return base + x; }
    double sum(double x) const { return base + x; }

    int base;
};

int main()
{
    using eular::reflection::Value;

    // 这个示例专门演示失败路径，帮助理解重载匹配和构造匹配规则。
    // 学习反射时，知道“为什么失败”比只看成功路径更重要。
    eular::reflection::class_<Calculator>("Calculator")
        .property("base", &Calculator::base)
        .method("sum", static_cast<int (Calculator::*)(int) const>(&Calculator::sum))
        .method("sum", static_cast<double (Calculator::*)(double) const>(&Calculator::sum))
        // 人工注册两条同签名重载，用于稳定复现 "overload ambiguous"。
        .method("sum_amb", static_cast<int (Calculator::*)(int) const>(&Calculator::sum))
        .method("sum_amb", static_cast<int (Calculator::*)(int) const>(&Calculator::sum))
        .constructor<>()
        .constructor<int>();

    std::shared_ptr<eular::reflection::TypeInfo> info =
        eular::reflection::Registry::instance().get("Calculator");

    if (!info) {
        std::cerr << "[fatal] type not found" << std::endl;
        return 1;
    }

    std::string err;

    // 1) 构造参数不匹配。
    // 这里故意传入两个参数，但注册里只有 0/1 参数构造，因此应当失败。
    Value bad_ctor;
    std::vector<Value> bad_ctor_args;
    bad_ctor_args.push_back(Value(1));
    bad_ctor_args.push_back(Value(2));

    bool ok = info->invoke_ctor(bad_ctor_args, bad_ctor, err);
    std::cout << "invoke_ctor(2 args) ok = " << ok << std::endl;
    std::cout << "err = " << err << std::endl;

    // 2) 正常构造一个实例。
    // 先拿到一个可用对象，再演示方法重载的歧义分支。
    Value obj_var;
    std::vector<Value> ctor_args;
    ctor_args.push_back(Value(20));
    ok = info->invoke_ctor(ctor_args, obj_var, err);
    if (!ok) {
        std::cerr << "[fatal] ctor failed: " << err << std::endl;
        return 1;
    }

    Calculator* calc = obj_var.get_value<Calculator*>();

    // 3) 重载歧义。
    // 这里人为注册两条完全相同的签名，让 invoke_best 稳定进入 ambiguous 分支。
    const eular::reflection::MethodInfo* sum_amb_method = info->find_method("sum_amb");
    if (!sum_amb_method) {
        std::cerr << "[fatal] method not found" << std::endl;
        return 1;
    }

    Value out;
    std::vector<Value> ambiguous_args;
    ambiguous_args.push_back(Value(3));

    err.clear();
    ok = sum_amb_method->invoke_best(calc, ambiguous_args, out, err);
    std::cout << "invoke sum_amb(3) ok = " << ok << std::endl;
    std::cout << "err = " << err << std::endl;

    // 4) 正常重载匹配。
    // 这一步作为对照，说明同一个方法组里也可以正常选中唯一签名。
    const eular::reflection::MethodInfo* sum_method = info->find_method("sum");
    if (!sum_method) {
        std::cerr << "[fatal] method not found" << std::endl;
        return 1;
    }

    std::vector<Value> good_args;
    good_args.push_back(Value(3));

    err.clear();
    ok = sum_method->invoke_best(calc, good_args, out, err);
    std::cout << "invoke sum(3) ok = " << ok << std::endl;
    if (ok && out.is_type<int>()) {
        std::cout << "result = " << out.get_value<int>() << std::endl;
    } else {
        std::cout << "err = " << err << std::endl;
    }

    // 5) 学习版析构。
    // 与成功示例一致，释放对象由反射层统一负责。
    err.clear();
    ok = info->destroy(obj_var, err);
    std::cout << "destroy ok = " << ok << std::endl;
    if (!ok) {
        std::cout << "err = " << err << std::endl;
    }

    return 0;
}
