/*************************************************************************
    > File Name: test_any.cc
    > Author: hsz
    > Brief:
    > Created Time: Fri 09 Dec 2022 03:52:20 PM CST
 ************************************************************************/

#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#include <string>

#include "catch/catch.hpp"
#include "utils/any.hpp"

template <size_t N>
struct words
{
    void *w[N];
};

struct big_type
{
    char i_wanna_be_big[256];
    std::string value;

    big_type() : value(std::string(300, 'b'))
    {
        i_wanna_be_big[0] = i_wanna_be_big[50] = 'k';
    }

    bool check()
    {
        REQUIRE(value.size() == 300);
        REQUIRE(value.front() == 'b');
        REQUIRE(value.back() == 'b');
        REQUIRE(i_wanna_be_big[0] == 'k');
        REQUIRE(i_wanna_be_big[50] == 'k');

        return true;
    }
};

// small type which has nothrow move ctor but throw copy ctor
struct regression1_type
{
    const void *confuse_stack_storage = (void *)(0);
    regression1_type() {}
    regression1_type(const regression1_type &) {}
    regression1_type(regression1_type &&) noexcept {}
    regression1_type &operator=(const regression1_type &) { return *this; }
    regression1_type &operator=(regression1_type &&) { return *this; }
};

TEST_CASE("simple test", "[any]") {
    eular::any int32_num = 10;
    eular::any uint32_num = 10u;
    eular::any int64_num = (int64_t)10;
    eular::any uint64_num = (uint64_t)10;
    eular::any double_num = 3.14;
    eular::any float_num = 3.14f;

    // NOTE 由于内部使用typeid返回值作为比较手段
    // long/long long
    // unsigned long/unsigned long long 
    // 无法相互转换
#ifndef ANY_IMPL_NO_RTTI
    if (typeid(int64_t) == typeid(long))
    {
        CHECK(eular::any(10L).type() == typeid(long));
    }

    if (typeid(int64_t) == typeid(long long))
    {
        CHECK(eular::any(10LL).type() == typeid(long long));
    }
#endif

    CHECK(eular::any_cast<int32_t>(&int32_num) != nullptr);
    CHECK(eular::any_cast<uint32_t>(&int32_num) == nullptr);

    CHECK(eular::any_cast<uint32_t>(&uint32_num) != nullptr);
    CHECK(eular::any_cast<int64_t>(&int64_num) != nullptr);
    CHECK(eular::any_cast<uint64_t>(&uint64_num) != nullptr);

    CHECK(eular::any_cast<double>(&double_num) != nullptr);
    CHECK(eular::any_cast<float>(&float_num) != nullptr);

    double *pDouble = eular::any_cast<double>(&double_num);
    float *pFloat = eular::any_cast<float>(&float_num);

    CHECK(*pDouble != *pFloat);
    // 浮点数比较无法直接判等
    CHECK(fabs(*pDouble - static_cast<double>(*pFloat)) < FLT_EPSILON);

    CHECK(eular::any().empty());
    CHECK(!eular::any(1).empty());
    CHECK(!eular::any(big_type()).empty());

    // not support array
    // int32_t array[2][3] = {
    //     {1, 2, 3},
    //     {4, 5, 6}
    // };
    // eular::any any_array = array;
    // CHECK(!any_array.empty());
    // decltype(array) *array_pointer = eular::any_cast<decltype(array)>(&any_array);
    // CHECK(*array_pointer != nullptr);
    // CHECK(*array_pointer[0][0] == 1);
    // CHECK(*array_pointer[0][1] == 2);
    // CHECK(*array_pointer[0][2] == 3);
    // CHECK(*array_pointer[1][0] == 4);
    // CHECK(*array_pointer[1][1] == 5);
    // CHECK(*array_pointer[1][2] == 6);
}

TEST_CASE("any_cast failed and threw an exception", "[any]") {
    bool except0 = false;
    bool except1 = false, except2 = false;
    bool except3 = false, except4 = false;

#ifndef ANY_IMPL_NO_EXCEPTIONS
    try
    {
        eular::any_cast<int>(eular::any());
    }
    catch (const eular::bad_any_cast &)
    {
        except0 = true;
    }

    try
    {
        eular::any_cast<int>(eular::any(4.0f));
    }
    catch (const eular::bad_any_cast &)
    {
        except1 = true;
    }

    try
    {
        eular::any_cast<float>(eular::any(4.0f));
    }
    catch (const eular::bad_any_cast &)
    {
        except2 = true;
    }

    try
    {
        eular::any_cast<float>(eular::any(big_type()));
    }
    catch (const eular::bad_any_cast &)
    {
        except3 = true;
    }

    try
    {
        eular::any_cast<big_type>(eular::any(big_type()));
    }
    catch (const eular::bad_any_cast &)
    {
        except4 = true;
    }
#endif

    CHECK(except0 == true);
    CHECK(except1 == true);
    CHECK(except2 == false);
    CHECK(except3 == true);
    CHECK(except4 == false);
}

TEST_CASE("shared_ptr and weak_ptr", "[any]") {
    std::shared_ptr<int> ptr_count(new int);
    std::weak_ptr<int> weak = ptr_count;
    eular::any p0 = 0;

    CHECK(weak.use_count() == 1);
    eular::any p1 = ptr_count;
    CHECK(weak.use_count() == 2);
    eular::any p2 = p1;
    CHECK(weak.use_count() == 3);
    p0 = p1;
    CHECK(weak.use_count() == 4);
    p0 = 0;
    CHECK(weak.use_count() == 3);
    p0 = std::move(p1);
    CHECK(weak.use_count() == 3);
    p0.swap(p1);
    CHECK(weak.use_count() == 3);
    p0 = 0;
    CHECK(weak.use_count() == 3);
    p1.clear();
    CHECK(weak.use_count() == 2);
    p2 = eular::any(big_type());
    CHECK(weak.use_count() == 1);
    p1 = ptr_count;
    CHECK(weak.use_count() == 2);
    ptr_count = nullptr;
    CHECK(weak.use_count() == 1);
    p1 = eular::any();
    CHECK(weak.use_count() == 0);
}

TEST_CASE("Is it on the stack", "[any]") {
    auto is_stack_allocated = [](const eular::any &a, const void *obj1) {
        uintptr_t a_ptr = (uintptr_t)(&a);
        uintptr_t obj = (uintptr_t)(obj1);
        return (obj >= a_ptr && obj < a_ptr + sizeof(eular::any));
    };

    static_assert(sizeof(std::shared_ptr<big_type>) <= sizeof(void *) * 2, "shared_ptr too big");

    eular::any i = 400;
    eular::any f = 400.0f;
    // any unique = std::unique_ptr<big_type>(); -- must be copy constructible
    eular::any shared = std::shared_ptr<big_type>();
    eular::any rawptr = (void *)(nullptr);
    eular::any big = big_type();
    eular::any w2 = words<2>();
    eular::any w3 = words<3>();

    CHECK(is_stack_allocated(i, eular::any_cast<int>(&i)));
    CHECK(is_stack_allocated(f, eular::any_cast<float>(&f)));
    CHECK(is_stack_allocated(rawptr, eular::any_cast<void *>(&rawptr)));
    CHECK(is_stack_allocated(shared, eular::any_cast<std::shared_ptr<big_type>>(&shared)));
    CHECK(!is_stack_allocated(big, eular::any_cast<big_type>(&big)));
    CHECK(is_stack_allocated(w2, eular::any_cast<words<2>>(&w2)));
    CHECK(!is_stack_allocated(w3, eular::any_cast<words<3>>(&w3)));

    // Regression test for GitHub Issue #1
    eular::any r1 = regression1_type();
    CHECK(is_stack_allocated(r1, eular::any_cast<regression1_type>(&r1)));
}