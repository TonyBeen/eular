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

#ifndef RTTR_TYPE_HPP_
#define RTTR_TYPE_HPP_

#include "variant/type.h"

#include "variant/detail/type/type_register_p.h"
#include "variant/detail/type/type_data.h"

#include <algorithm>
#include <unordered_map>
#include <vector>
#include <memory>
#include <set>
#include <thread>
#include <mutex>
#include <cstring>
#include <cctype>
#include <utility>

using namespace std;

namespace rttr
{

/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////

type::type()
:   m_type_data(detail::get_invalid_type_data())
{
}

/////////////////////////////////////////////////////////////////////////////////////////

type type::get_by_name(const std::string &name)
{
    auto& custom_name_to_id = detail::type_register_private::get_instance().get_custom_name_to_id();
    auto ret = custom_name_to_id.find(name);
    if (ret != custom_name_to_id.end())
        return (*ret);

    return detail::get_invalid_type();
}

/////////////////////////////////////////////////////////////////////////////////////////

void type::create_wrapped_value(const argument& arg, variant& var) const
{
    if (m_type_data->create_wrapper)
        m_type_data->create_wrapper(arg, var);
}

} // end namespace rttr

#endif // RTTR_TYPE_HPP_
