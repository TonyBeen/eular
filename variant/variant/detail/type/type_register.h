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

#ifndef RTTR_TYPE_REGISTER_H_
#define RTTR_TYPE_REGISTER_H_

#include <memory>
#include <string>
#include <vector>

namespace rttr
{
class variant;
class type;
class argument;

template<typename T>
class class_;

namespace detail
{
struct base_class_info;
struct derived_info;

using variant_create_func   = variant(*)(const argument&);
using get_derived_func      = derived_info(*)(void*);

template<typename T, typename Enable>
struct type_getter;

struct type_data;

class registration_manager;

/*!
 * This class contains all functions to register properties, methods etc.. for a specific type.
 * This is a static pimpl, it will just forward the data to the \ref type_register_private class.
 */
class type_register
{
public:
    // no copy
    type_register(const type_register&) = delete;
    // no assign
    type_register& operator=(const type_register&) = delete;

    static void custom_name(type& t, const std::string &name);

    static void register_reg_manager(registration_manager* manager);
    static void unregister_reg_manager(registration_manager* manager);

    static type_data* register_type(type_data* info);
    static void unregister_type(type_data* info);

private:

    friend class type;
    template<typename T>
    friend class class_;

    template<typename T, typename Enable>
    friend struct detail::type_getter;
};

} // end namespace detail
} // end namespace rttr

#include "variant/detail/type/type_register.hpp"

#endif // RTTR_TYPE_REGISTER_H_
