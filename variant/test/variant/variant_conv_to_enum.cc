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

#include "variant/test_enums.h"
#include <variant/variant.h>
#include <variant/type.h>

using namespace rttr;

/////////////////////////////////////////////////////////////////////////////////////////

TEST_CASE("variant::to enum - from empty", "[variant]")
{
    variant var;
    bool ok = false;
    CHECK(var.convert(type::get<variant_enum_test>()) == false);
    var.convert<variant_enum_test>(&ok);
    CHECK(ok == false);

    variant_enum_test value;
    CHECK(var.convert(value) == false);
}

/////////////////////////////////////////////////////////////////////////////////////////

TEST_CASE("variant:to enum - from bool", "[variant]")
{
    SECTION("valid conversion")
    {
        variant var = true;
        REQUIRE(var.is_valid() == true);
        REQUIRE(var.can_convert<variant_enum_test>() == false);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////

TEST_CASE("variant:to enum - from float", "[variant]")
{
    SECTION("valid conversion positive")
    {
        variant var = 2.0f;
        REQUIRE(var.can_convert<variant_enum_test>() == false);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////

TEST_CASE("variant:to enum - from int32_t", "[variant]")
{
    SECTION("valid conversion positive")
    {
        variant var = int32_t(2);
        REQUIRE(var.can_convert<variant_enum_test>() == false);
    }
}
