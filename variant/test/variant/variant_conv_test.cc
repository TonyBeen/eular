/************************************************************************************
*                                                                                   *
*   Copyright (c) 2014 - 2018 Axel Menzel <info@rttr.org>                           *
*                                                                                   *
*   This file is part of RTTR (Run Time Type Reflection)                            *
*   License: MIT License                                                            *
*                                                                                   *
*   Permission is hereby granted, free of charge, to any person obtaining           *
*   a copy of this software and associated documentation files (the "Software"),    *
*   to deal in the Software without restriction, including without limitation       *
*   the rights to use, copy, modify, merge, publish, distribute, sublicense,        *
*   and/or sell copies of the Software, and to permit persons to whom the           *
*   Software is furnished to do so, subject to the following conditions:            *
*                                                                                   *
*   The above copyright notice and this permission notice shall be included in      *
*   all copies or substantial portions of the Software.                             *
*                                                                                   *
*   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR      *
*   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,        *
*   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE     *
*   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER          *
*   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,   *
*   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE   *
*   SOFTWARE.                                                                       *
*                                                                                   *
*************************************************************************************/

#include <catch/catch.hpp>

#include <variant/variant.h>
#include <variant/type.h>

using namespace rttr;

struct lifetime_probe
{
    lifetime_probe(int v) : value(v) {}

    ~lifetime_probe()
    {
        ++destructor_count;
    }

    static void reset_counters()
    {
        destructor_count = 0;
        delete_count = 0;
    }

    static void operator delete(void* ptr) noexcept
    {
        ++delete_count;
        ::operator delete(ptr);
    }

    static int destructor_count;
    static int delete_count;
    int value;
};

int lifetime_probe::destructor_count = 0;
int lifetime_probe::delete_count = 0;

struct point
{
    point(int x, int y) : _x(x), _y(y) {}

    point(const point& other) : _x(other._x), _y(other._y) { }

    point(point&& other) : _x(other._x), _y(other._y) { other._x = 0; other._y = 0; }

    bool operator ==(const point& other) const { return (_x == other._x && _y == other._y); }
    int _x;
    int _y;
};

template<class T>
static
typename std::enable_if<!std::numeric_limits<T>::is_integer, bool>::type
almost_equal(T x, T y)
{
    return std::abs(x - y) <= std::numeric_limits<T>::epsilon();
}

struct vector2d
{
    vector2d(int x, int y) : _x(x), _y(y) {}
    int _x;
    int _y;
};

/////////////////////////////////////////////////////////////////////////////////////////

TEST_CASE("variant - basic - can_convert()", "[variant]")
{
    variant var;
    CHECK(var.can_convert(type::get<int>()) == false);
}

/////////////////////////////////////////////////////////////////////////////////////////

TEST_CASE("variant conversion - to int", "[variant]")
{
    SECTION("int to int")
    {
        variant var = 12;
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<int>() == true);

        CHECK(var.to_int() == 12);
        CHECK(var.convert<int>() == 12);
        CHECK(var.convert(type::get<int>()) == true);
        CHECK(var.get_value<int>() == 12);

        var = -23;
        CHECK(var.to_int() == -23);
        CHECK(var.convert<int>() == -23);
        CHECK(var.convert(type::get<int>()) == true);
        CHECK(var.get_value<int>() == -23);
    }

    SECTION("char to int")
    {
        variant var = "23";
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<int>() == true);
        CHECK(var.can_convert(type::get<int>()) == true);

        CHECK(var.to_int() == 23);
        CHECK(var.convert<int>() == 23);
        CHECK(var.convert(type::get<int>()) == true);
        CHECK(var.get_value<int>() == 23);

        var = "-12";
        CHECK(var.to_int() == -12);

        var = "text 34 and text";
        bool ok = false;
        CHECK(var.to_int(&ok) == 0);
        CHECK(ok == false);

        var = "34 and text";
        ok = false;
        CHECK(var.to_int(&ok) == 0);
        CHECK(ok == false);
        CHECK(var.convert<int>() == 0);
        CHECK(var.convert(type::get<int>()) == false);
    }

    SECTION("std::string to int")
    {
        variant var = std::string("23");
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<int>() == true);

        CHECK(var.to_int() == 23);
        CHECK(var.convert<int>() == 23);
        CHECK(var.convert(type::get<int>()) == true);
        CHECK(var.get_value<int>() == 23);

        var = std::string("-12");
        CHECK(var.to_int() == -12);

        var = std::string("text 34 and text");
        bool ok = false;
        CHECK(var.to_int(&ok) == 0);
        CHECK(ok == false);

        var = std::string("34 and text");
        ok = false;
        CHECK(var.to_int(&ok) == 0);
        CHECK(ok == false);
    }

    SECTION("bool to int")
    {
        variant var = true;
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<int>() == true);

        CHECK(var.to_int() == 1);

        var = false;
        CHECK(var.to_int() == 0);
    }

    SECTION("float to int")
    {
        variant var = 1.5f;
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<int>() == true);

        CHECK(var.to_int() == 1);

        var = 3.1423f;
        CHECK(var.to_int() == 3);

        var = 0.0f;
        CHECK(var.to_int() == 0);
    }

    SECTION("double to int")
    {
        variant var = 1.5;
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<int>() == true);

        CHECK(var.to_int() == 1);

        var = 3.1423f;
        CHECK(var.to_int() == 3);

        var = 0.0;
        CHECK(var.to_int() == 0);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////

TEST_CASE("variant conversion - to std::string", "[variant]")
{
    SECTION("int to std::string")
    {
        variant var = 12;
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<std::string>() == true);

        CHECK(var.to_string() == "12");
        CHECK(var.convert<std::string>() == "12");
        CHECK(var.convert(type::get<std::string>()) == true);
        CHECK(var.get_value<std::string>() == "12");

        var = -23;
        CHECK(var.to_string() == "-23");
    }

    SECTION("char to std::string")
    {
        variant var = "text";
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<std::string>() == true);

        CHECK(var.to_string() == "text");

        var = "text 42";
        CHECK(var.to_string() == "text 42");
    }

    SECTION("std::string to std::string")
    {
        variant var = std::string("23");
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<std::string>() == true);

        CHECK(var.to_string() == "23");

        var = std::string("-12");
        CHECK(var.to_string() == "-12");
    }

    SECTION("bool to std::string")
    {
        variant var = true;
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<std::string>() == true);

        CHECK(var.to_string() == "true");

        var = false;
        CHECK(var.to_string() == "false");
    }

    SECTION("float to std::string")
    {
        variant var = 1.567f;
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<std::string>() == true);

        CHECK(var.to_string() == "1.567");

        var = 3.12345678f;
        CHECK(var.to_string() == "3.12346");

        var = 0.0f;
        CHECK(var.to_string() == "0");
    }

    SECTION("double to std::string")
    {
        variant var = 1.567;
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<std::string>() == true);

        CHECK(var.to_string() == "1.567");

        var = 3.12345678;
        CHECK(var.to_string() == "3.12345678");

        var = 0.0;
        CHECK(var.to_string() == "0");
    }
}

/////////////////////////////////////////////////////////////////////////////////////////

TEST_CASE("variant conversion - to float", "[variant]")
{
    SECTION("int to float")
    {
        variant var = 12;
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<float>() == true);

        CHECK(var.to_float() == 12.0f);
        CHECK(var.convert<float>() == 12.0f);
        CHECK(var.convert(type::get<float>()) == true);
        CHECK(var.get_value<float>() == 12.0f);

        var = -23;
        CHECK(var.to_float() == -23.0f);
    }

    SECTION("char to float")
    {
        variant var = "23.0";
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<float>() == true);

        CHECK(var.to_float() == 23.0f);
        CHECK(var.convert<float>() == 23.0f);
        CHECK(var.convert(type::get<float>()) == true);
        CHECK(var.get_value<float>() == 23.0f);

        var = "text 42";
        bool ok = false;
        CHECK(var.to_float(&ok) == 0);
        CHECK(ok == false);

        var = "1.23456";
        ok = false;
        CHECK(var.to_float(&ok) == 1.23456f);
        CHECK(ok == true);

        var = "1.23456 Text";
        ok = false;
        CHECK(var.to_float(&ok) == 0);
        CHECK(ok == false);

    }

    SECTION("std::string to float")
    {
        variant var = std::string("23.0");
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<float>() == true);

        CHECK(var.to_float() == 23.0f);

        var = std::string("text 42");
        bool ok = false;
        CHECK(var.to_float(&ok) == 0);
        CHECK(ok == false);

        var = std::string("1.23456");
        ok = false;
        CHECK(var.to_float(&ok) == 1.23456f);
        CHECK(ok == true);

        var = std::string("1.23456 Text");
        ok = false;
        CHECK(var.to_float(&ok) == 0);
        CHECK(ok == false);
    }

    SECTION("bool to float")
    {
        variant var = true;
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<float>() == true);

        CHECK(var.to_float() == 1.0f);

        var = false;
        CHECK(var.to_float() == 0.0f);
    }

    SECTION("float to float")
    {
        variant var = 1.567f;
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<float>() == true);

        CHECK(almost_equal(var.to_float(), 1.567f) == true);

        var = 3.12345678f;
        CHECK(almost_equal(var.to_float(), 3.1234567f) == true);

        var = 0.0f;
        CHECK(almost_equal(var.to_float(), 0.0f) == true);
    }

    SECTION("double to float")
    {
        variant var = 1.567;
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<float>() == true);

        CHECK(almost_equal(var.to_float(), 1.567f) == true);

        var = 3.12345678;
        CHECK(almost_equal(var.to_float(), 3.1234567f) == true);

        var = 0.0;
        CHECK(almost_equal(var.to_float(), 0.0f) == true);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////

TEST_CASE("variant conversion - to double", "[variant]")
{
    SECTION("int to double")
    {
        variant var = 12;
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<double>() == true);

        CHECK(var.to_double() == 12.0);
        CHECK(var.convert<double>() == 12.0);
        CHECK(var.convert(type::get<double>()) == true);
        CHECK(var.get_value<double>() == 12.0);

        var = -23;
        CHECK(var.to_double() == -23.0);
    }

    SECTION("char to double")
    {
        variant var = "23.0";
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<double>() == true);

        CHECK(var.to_double() == 23.0);

        var = "text 42";
        bool ok = false;
        CHECK(var.to_double(&ok) == 0);
        CHECK(ok == false);

        var = "1.23456";
        ok = false;
        CHECK(var.to_double(&ok) == 1.23456);
        CHECK(ok == true);

        var = "1.23456 Text";
        ok = false;
        CHECK(var.to_double(&ok) == 0.0);
        CHECK(ok == false);
    }

    SECTION("std::string to double")
    {
        variant var = std::string("23.0");
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<double>() == true);

        REQUIRE(var.to_double() == 23.0);

        var = std::string("text 42");
        bool ok = false;
        CHECK(var.to_double(&ok) == 0.0);
        CHECK(ok == false);

        var = std::string("1.23456");
        ok = false;
        CHECK(var.to_double(&ok) == 1.23456);
        CHECK(ok == true);

        var = std::string("1.23456 Text");
        ok = false;
        CHECK(var.to_double(&ok) == 0.0);
        CHECK(ok == false);
    }

    SECTION("bool to double")
    {
        variant var = true;
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<double>() == true);

        CHECK(var.to_double() == 1.0);

        var = false;
        CHECK(var.to_double() == 0.0);
    }

    SECTION("float to double")
    {
        variant var = 1.567f;
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<double>() == true);

        CHECK(almost_equal(var.to_double(), 1.5670000314712524) == true);

        var = 3.123456f;
        CHECK(almost_equal(var.to_double(), 3.1234560012817383) == true);

        var = 0.0f;
        CHECK(almost_equal(var.to_double(), 0.0) == true);
    }

    SECTION("double to double")
    {
        variant var = 1.567;
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<double>() == true);

        CHECK(almost_equal(var.to_double(), 1.567) == true);

        var = 3.12345678;
        CHECK(almost_equal(var.to_double(), 3.12345678) == true);

        var = 0.0;
        CHECK(almost_equal(var.to_double(), 0.0) == true);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////

TEST_CASE("variant test - convert to nullptr", "[variant]")
{
    SECTION("Invalid conversion")
    {
        int obj = 42;
        variant var = &obj;

        CHECK(var.can_convert(type::get<std::nullptr_t>()) == false);

        bool ok = false;
        var.convert<std::nullptr_t>(&ok);
        CHECK(ok == false);

        CHECK(var.convert(type::get<std::nullptr_t>()) == false);

        std::nullptr_t null_obj;
        CHECK(var.convert<std::nullptr_t>(null_obj) == false);
    }

    SECTION("valid conversion")
    {
        int* obj = nullptr;
        variant var = obj;

        CHECK(var.can_convert(type::get<std::nullptr_t>()) == true);
        std::nullptr_t null_obj;
        CHECK(var.convert<std::nullptr_t>(null_obj) == true);
        CHECK(null_obj == nullptr);

        bool ok = false;
        var.convert<std::nullptr_t>(&ok);
        CHECK(ok == true);

        CHECK(var.convert(type::get<std::nullptr_t>()) == true);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////

TEST_CASE("variant test - convert from wrapped value", "[variant]")
{
    SECTION("valid conversion")
    {
        int obj = 42;
        variant var = std::ref(obj);

        CHECK(var.can_convert(type::get<int>()) == true);

        bool ok = false;
        int val = var.convert<int>(&ok);
        CHECK(ok == true);
        CHECK(val == obj);

        CHECK(var.convert(type::get<int>()) == true);

        int val_2;
        CHECK(var.convert<int>(val_2) == true);
        CHECK(val_2 == obj);
    }

    SECTION("valid conversion std::shared_ptr")
    {
        auto raw_ptr = new int(42);
        variant var = std::shared_ptr<int>(raw_ptr);

        CHECK(var.can_convert(type::get<int*>()) == true);

        bool ok = false;
        auto val = var.convert<int*>(&ok);
        CHECK(ok == true);
        CHECK(val == raw_ptr);

        int* val_2;
        CHECK(var.convert<int*>(val_2) == true);
        CHECK(val_2 == raw_ptr);

        CHECK(var.convert(type::get<int*>()) == true);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////

TEST_CASE("variant test - convert to wrapped value", "[variant]")
{
    SECTION("raw pointer cannot convert to shared_ptr in place")
    {
        auto raw_ptr = new int(42);
        variant var = raw_ptr;

        REQUIRE(var.can_convert(type::get<std::shared_ptr<int>>()) == false);

        auto result = var.convert(type::get<std::shared_ptr<int>>());
        CHECK(result == false);
        CHECK(var.get_type() == type::get<int*>());

        delete raw_ptr;
    }

    SECTION("raw pointer cannot convert and return shared_ptr")
    {
        auto raw_ptr = new int(42);
        variant var = raw_ptr;

        CHECK(var.can_convert<std::shared_ptr<int>>() == false);

        bool ok = false;
        auto ptr = var.convert<std::shared_ptr<int>>(&ok);
        CHECK(ok == false);
        CHECK(ptr.get() == nullptr);
        CHECK(var.get_type() == type::get<int*>());

        delete raw_ptr;
    }

    SECTION("raw pointer cannot convert to existing shared_ptr")
    {
        auto raw_ptr = new int(42);
        variant var = raw_ptr;
        std::shared_ptr<int> ptr;
        CHECK(var.convert(ptr)  == false);
        CHECK(ptr.get()         == nullptr);
        CHECK(var.get_type()    == type::get<int*>());

        delete raw_ptr;
    }

    SECTION("failed shared_ptr conversion does not delete raw pointer")
    {
        lifetime_probe::reset_counters();
        auto raw_ptr = new lifetime_probe(42);

        {
            variant var = raw_ptr;

            bool ok = false;
            auto ptr = var.convert<std::shared_ptr<lifetime_probe>>(&ok);

            REQUIRE(ok == false);
            REQUIRE(ptr.get() == nullptr);
            CHECK(lifetime_probe::destructor_count == 0);
            CHECK(lifetime_probe::delete_count == 0);
        }

        CHECK(lifetime_probe::destructor_count == 0);
        CHECK(lifetime_probe::delete_count == 0);

        delete raw_ptr;
        CHECK(lifetime_probe::destructor_count == 1);
        CHECK(lifetime_probe::delete_count == 1);
    }

    SECTION("failed in-place shared_ptr conversion leaves raw pointer untouched")
    {
        lifetime_probe::reset_counters();
        auto raw_ptr = new lifetime_probe(7);

        {
            variant var = raw_ptr;

            REQUIRE(var.convert(type::get<std::shared_ptr<lifetime_probe>>()) == false);
            REQUIRE(var.get_type() == type::get<lifetime_probe*>());
            CHECK(lifetime_probe::destructor_count == 0);
            CHECK(lifetime_probe::delete_count == 0);
        }

        CHECK(lifetime_probe::destructor_count == 0);
        CHECK(lifetime_probe::delete_count == 0);

        delete raw_ptr;
        CHECK(lifetime_probe::destructor_count == 1);
        CHECK(lifetime_probe::delete_count == 1);
    }

    SECTION("invalid conversion")
    {
        int obj = 42;
        variant var = obj;

        CHECK(var.can_convert(type::get<std::unique_ptr<int>>())        == false);
        CHECK(var.can_convert(type::get<std::reference_wrapper<int>>()) == false);
        CHECK(var.can_convert(type::get<std::weak_ptr<int>>())          == false);
        CHECK(var.can_convert(type::get<std::shared_ptr<int>>())        == false);

        var = &obj;

        CHECK(var.can_convert(type::get<std::unique_ptr<int>>())        == false);
        CHECK(var.can_convert(type::get<std::reference_wrapper<int>>()) == false);
        CHECK(var.can_convert(type::get<std::weak_ptr<int>>())          == false);
        // bool is wrong wrapped type!
        CHECK(var.can_convert(type::get<std::shared_ptr<bool>>())       == false);

        auto result = var.convert(type::get<std::shared_ptr<bool>>());
        CHECK(result == false);
    }
}
