#include "reflection/type_info.h"
#include "reflection/mini_reflect.h"

#include <cassert>
#include <string>
#include <vector>

struct Person {
    Person() : age(0), destroyed(false) {}
    explicit Person(int a) : age(a), destroyed(false) {}
    ~Person() { destroyed = true; }

    int sum() const { return age; }
    int add(int v) const { return age + v; }
    double add(double v) const { return age + v; }

    int age;
    bool destroyed;
};

int main()
{
    using eular::reflection::Registry;
    using eular::reflection::TypeInfo;
    using eular::reflection::Value;

    eular::reflection::class_<Person>("Person")
        .property("age", &Person::age)
        .method("add", static_cast<int (Person::*)(int) const>(&Person::add))
        .method("add", static_cast<double (Person::*)(double) const>(&Person::add))
        .method("sum", &Person::sum)
        .constructor<>()
        .constructor<int>();

    std::shared_ptr<TypeInfo> info = Registry::instance().get("Person");
    assert(info);

    Person p(10);

    const eular::reflection::PropertyInfo* age_prop = info->find_property("age");
    assert(age_prop != NULL);

    Value age_out;
    std::string err;
    bool ok = age_prop->get(&p, age_out, err);
    assert(ok);
    assert(age_out.is_type<int>());
    assert(age_out.get_value<int>() == 10);

    ok = age_prop->set(&p, Value(18), err);
    assert(ok);
    assert(p.age == 18);

    ok = age_prop->set(&p, Value(Person()), err);
    assert(!ok);

    const eular::reflection::MethodInfo* add_method = info->find_method("add");
    assert(add_method != NULL);

    Value invoke_out;
    ok = add_method->invoke_best(&p, std::vector<Value>(1, Value(2)), invoke_out, err);
    assert(ok);
    assert(invoke_out.is_type<int>());
    assert(invoke_out.get_value<int>() == 20);

    ok = add_method->invoke_best(&p, std::vector<Value>(1, Value(2.5)), invoke_out, err);
    assert(ok);
    assert(invoke_out.is_type<double>());
    assert(invoke_out.get_value<double>() == 20.5);

    // bool 同时可转 int 与 double，此处用于验证重载歧义分支。
    ok = add_method->invoke_best(&p, std::vector<Value>(1, Value(true)), invoke_out, err);
    assert(!ok);

    Value ctor_obj;
    ok = info->invoke_ctor(std::vector<Value>(1, Value(7)), ctor_obj, err);
    assert(ok);
    assert(ctor_obj.is_type<Person*>());

    Person* created = ctor_obj.get_value<Person*>();
    assert(created != NULL);
    assert(created->age == 7);

    ok = info->destroy(ctor_obj, err);
    assert(ok);
    assert(!ctor_obj.is_valid());

    Value bad_instance = 123;
    ok = info->destroy(bad_instance, err);
    assert(!ok);

    return 0;
}
