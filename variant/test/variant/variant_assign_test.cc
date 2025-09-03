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

/////////////////////////////////////////////////////////////////////////////////////////

TEST_CASE("basic assignment", "[variant]")
{
    SECTION("empty type")
    {
        variant a;
        variant b;

        a = b;
        CHECK(a.is_valid() == false);
        CHECK((bool)a      == false);
        CHECK(a.get_type().is_valid() == false);

        a = std::move(b);
        CHECK(a.is_valid() == false);
        CHECK((bool)a      == false);
        CHECK(a.get_type().is_valid() == false);

        CHECK(b.is_valid() == false);
        CHECK((bool)b      == false);
        CHECK(b.get_type().is_valid() == false);
    }

    SECTION("insert arithmetic type")
    {
        variant a;
        variant b(1);

        a = b;

        CHECK(a.is_valid() == true);
        CHECK((bool)a      == true);
        CHECK(a.get_type().is_valid() == true);
        CHECK(a.get_type() == type::get<int>());

        a = std::move(b);

        CHECK(a.is_valid() == true);
        CHECK((bool)a      == true);
        CHECK(a.get_type().is_valid() == true);
        CHECK(a.get_type() == type::get<int>());

        CHECK(b.is_valid() == false);
        CHECK((bool)b      == false);
        CHECK(b.get_type().is_valid() == false);

        a = b;
        CHECK(a.is_valid() == false);
        CHECK((bool)a      == false);
        CHECK(a.get_type().is_valid() == false);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////

TEST_CASE("variant::operator=() - self assignment", "[variant]")
{
    SECTION("self assign - empty")
    {
        variant a;
        a = a;

        CHECK(a.is_valid() == false);
    }

    SECTION("self assign - full")
    {
        variant a = 1;
        a = a;

        CHECK(a.is_valid() == true);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////
