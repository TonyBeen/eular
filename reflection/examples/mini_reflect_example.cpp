#include "reflection/mini_reflect.h"

#include <iostream>
#include <vector>

struct Person {
    Person() : age(0), level(1.0) {}
    Person(int a, double l) : age(a), level(l) {}

    int score() const { return static_cast<int>(age * level); }
    int add(int x) const { return age + x; }
    double add(double x) const { return age + x; }

    int age;
    double level;
};

int main()
{
    using eular::reflection::TypeInfo;
    using eular::reflection::Value;

    // 1) 注册类型。
    // 这里先注册 public 成员，后面你可以再把它改造成 getter/setter 版本。
    eular::reflection::class_<Person>("Person")
        .property("age", &Person::age)
        .property("level", &Person::level)
        .method("score", &Person::score)
        .method("add", static_cast<int (Person::*)(int) const>(&Person::add))
        .method("add", static_cast<double (Person::*)(double) const>(&Person::add))
        .constructor<>()
        .constructor<int, double>();

    std::shared_ptr<TypeInfo> info = eular::reflection::Registry::instance().get("Person");
    if (!info) {
        std::cerr << "type not found" << std::endl;
        return 1;
    }

    std::string err;

    // 2) 通过反射构造对象。
    // invoke_ctor 会按构造函数签名匹配参数，并返回一个装着 Person* 的 Value。
    Value instance_var;
    std::vector<Value> ctor_args;
    ctor_args.push_back(Value(20));
    ctor_args.push_back(Value(1.5));

    if (!info->invoke_ctor(ctor_args, instance_var, err)) {
        std::cerr << err << std::endl;
        return 1;
    }

    Person* person = instance_var.get_value<Person*>();

    // 3) 读取属性。
    // 反射层只负责调度，真正的成员访问逻辑仍然落在注册时绑定的 lambda 上。
    const eular::reflection::PropertyInfo* age_prop = info->find_property("age");
    Value age_value;
    if (!age_prop->get(person, age_value, err)) {
        std::cerr << err << std::endl;
        return 1;
    }
    std::cout << "age = " << age_value.get_value<int>() << std::endl;

    // 4) 写入属性。
    // 这里传入 int，属性本身也是 int，所以不需要额外转换。
    if (!age_prop->set(person, Value(25), err)) {
        std::cerr << err << std::endl;
        return 1;
    }
    std::cout << "age(after set) = " << person->age << std::endl;

    // 5) 调用普通方法。
    // 无参数方法不需要构造额外的 Value 列表。
    const eular::reflection::MethodInfo* score_method = info->find_method("score");
    Value score_out;
    if (!score_method->invoke_best(person, std::vector<Value>(), score_out, err)) {
        std::cerr << err << std::endl;
        return 1;
    }
    std::cout << "score = " << score_out.get_value<int>() << std::endl;

    // 6) 调用重载方法。
    // 同名方法会被聚合成一个重载组，invoke_best 负责挑选最合适的签名。
    const eular::reflection::MethodInfo* add_method = info->find_method("add");
    Value add_out;

    if (!add_method->invoke_best(person, {22}, add_out, err)) {
        std::cerr << err << std::endl;
        return 1;
    }
    std::cout << "add(int) = " << add_out.get_value<int>() << std::endl;

    if (!add_method->invoke_best(person, {3.5}, add_out, err)) {
        std::cerr << err << std::endl;
        return 1;
    }
    Value temp = add_out;
    if (!temp.convert(eular::reflection::Type::get<int32_t>())) {
        std::cerr << "convert failed" << std::endl;
        return 1;
    }
    std::cout << "add(double) = " << temp.get_value<int32_t>() << std::endl;

    // 7) 学习版析构。
    // 当前版本把“谁创建谁销毁”这件事统一收敛到 destroy，便于理解生命周期。
    if (!info->destroy(instance_var, err)) {
        std::cerr << err << std::endl;
        return 1;
    }

    std::cout << "mini reflection example done" << std::endl;
    return 0;
}
