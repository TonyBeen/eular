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

#ifndef RTTR_TYPE_IMPL_H_
#define RTTR_TYPE_IMPL_H_

#include <type_traits>
#include "variant/detail/misc/misc_type_traits.h"
#include "variant/detail/misc/function_traits.h"
#include "variant/detail/type/base_classes.h"
#include "variant/detail/type/get_derived_info_func.h"
#include "variant/detail/type/get_create_variant_func.h"
#include "variant/detail/type/type_register.h"
#include "variant/detail/misc/utility.h"
#include "variant/detail/type/type_data.h"
#include "variant/detail/type/type_name.h"
#include "variant/detail/registration/registration_manager.h"
#include "variant/detail/misc/register_wrapper_mapper_conversion.h"

namespace rttr
{

/////////////////////////////////////////////////////////////////////////////////////////

type::type(detail::type_data* data)
:  m_type_data(data)
{
}

/////////////////////////////////////////////////////////////////////////////////////////

type::type(const type& other)
:   m_type_data(other.m_type_data)
{
}

/////////////////////////////////////////////////////////////////////////////////////////

type& type::operator=(const type& other)
{
    m_type_data = other.m_type_data;
    return *this;
}

/////////////////////////////////////////////////////////////////////////////////////////

bool type::operator<(const type& other) const
{
    return (m_type_data < other.m_type_data);
}

/////////////////////////////////////////////////////////////////////////////////////////

bool type::operator>(const type& other) const
{
    return (m_type_data > other.m_type_data);
}

/////////////////////////////////////////////////////////////////////////////////////////

bool type::operator>=(const type& other) const
{
    return (m_type_data >= other.m_type_data);
}

/////////////////////////////////////////////////////////////////////////////////////////

bool type::operator<=(const type& other) const
{
    return (m_type_data <= other.m_type_data);
}

/////////////////////////////////////////////////////////////////////////////////////////

bool type::operator==(const type& other) const
{
    return (m_type_data == other.m_type_data);
}

/////////////////////////////////////////////////////////////////////////////////////////

bool type::operator!=(const type& other) const
{
    return (m_type_data != other.m_type_data);
}

/////////////////////////////////////////////////////////////////////////////////////////

type::type_id type::get_id() const
{
    return reinterpret_cast<type::type_id>(m_type_data);
}

/////////////////////////////////////////////////////////////////////////////////////////

bool type::is_valid() const
{
    return m_type_data->is_valid;
}

/////////////////////////////////////////////////////////////////////////////////////////

type::operator bool() const
{
    return m_type_data->is_valid;
}

/////////////////////////////////////////////////////////////////////////////////////////

type type::get_raw_type() const
{
    return type(m_type_data->raw_type_data);
}

/////////////////////////////////////////////////////////////////////////////////////////

type type::get_wrapped_type() const
{
    return type(m_type_data->wrapped_type);
}

/////////////////////////////////////////////////////////////////////////////////////////

std::string type::get_name() const
{
    return m_type_data->type_name;
}

/////////////////////////////////////////////////////////////////////////////////////////

std::size_t type::get_sizeof() const
{
    return m_type_data->get_sizeof;
}

/////////////////////////////////////////////////////////////////////////////////////////

bool type::is_class() const
{
    return m_type_data->type_trait_value(detail::type_trait_infos::is_class);
}

/////////////////////////////////////////////////////////////////////////////////////////

bool type::is_template_instantiation() const
{
    return m_type_data->type_trait_value(detail::type_trait_infos::is_template_instantiation);
}

/////////////////////////////////////////////////////////////////////////////////////////

bool type::is_enumeration() const
{
    return m_type_data->type_trait_value(detail::type_trait_infos::is_enum);
}

/////////////////////////////////////////////////////////////////////////////////////////

bool type::is_array() const
{
    return m_type_data->type_trait_value(detail::type_trait_infos::is_array);
}

/////////////////////////////////////////////////////////////////////////////////////////

bool type::is_associative_container() const
{
    return m_type_data->type_trait_value(detail::type_trait_infos::is_associative_container);
}

/////////////////////////////////////////////////////////////////////////////////////////

bool type::is_sequential_container() const
{
    return m_type_data->type_trait_value(detail::type_trait_infos::is_sequential_container);
}

/////////////////////////////////////////////////////////////////////////////////////////

bool type::is_pointer() const
{
    return m_type_data->type_trait_value(detail::type_trait_infos::is_pointer);
}

/////////////////////////////////////////////////////////////////////////////////////////

bool type::is_arithmetic() const
{
    return m_type_data->type_trait_value(detail::type_trait_infos::is_arithmetic);
}

/////////////////////////////////////////////////////////////////////////////////////////

bool type::is_wrapper() const
{
    return m_type_data->wrapped_type->is_valid;
}


/////////////////////////////////////////////////////////////////////////////////////////

variant type::create_variant(const argument& data) const
{
    return m_type_data->create_variant(data);
}

/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////

namespace detail
{

static type get_invalid_type() { return create_type(nullptr); }

/////////////////////////////////////////////////////////////////////////////////////////

type create_type(type_data* data)
{
    return data ? type(data) : type();
}

/////////////////////////////////////////////////////////////////////////////////
template<typename T>
using is_complete_type = std::integral_constant<bool, !std::is_function<T>::value && !std::is_same<T, void>::value>;

template<typename T>
enable_if_t<is_complete_type<T>::value, type>
create_or_get_type()
{
    // when you get an error here, then the type was not completely defined
    // (a forward declaration is not enough because base_classes will not be found)
    using type_must_be_complete = char[ sizeof(T) ? 1: -1 ];
    (void) sizeof(type_must_be_complete);
    static const type val = create_type(get_registration_manager().add_item(make_type_data<T>()));
    return val;
}

/////////////////////////////////////////////////////////////////////////////////

template<typename T>
enable_if_t<!is_complete_type<T>::value, type>
create_or_get_type()
{
    static const type val = create_type(get_registration_manager().add_item(make_type_data<T>()));
    return val;
}

/////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////

template<typename T>
type get_type_from_instance(const T*)
{
    return detail::create_or_get_type<T>();
}

/////////////////////////////////////////////////////////////////////////////////

template<typename T, bool>
struct type_from_instance;

//! Specialization for retrieving the type from the instance directly
template<typename T>
struct type_from_instance<T, false> // the typeInfo function is not available
{
    static type get(T&&)
    {
        using non_ref_type = typename std::remove_cv<typename std::remove_reference<T>::type>::type;
        return create_or_get_type<non_ref_type>();
    }
};

/////////////////////////////////////////////////////////////////////////////////////////

//! Specialization for retrieving the type from the instance directly
template<typename T>
struct type_from_instance<T, true>
{
    static type get(T&& object)
    {
        return object.get_type();
    }
};

/////////////////////////////////////////////////////////////////////////////////////////

template<typename TargetType, typename SourceType, typename F>
struct type_converter;

} // end namespace detail

/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////

template<typename T>
type type::get()
{
    using non_ref_type = typename std::remove_cv<typename std::remove_reference<T>::type>::type;
    return detail::create_or_get_type<non_ref_type>();
}

/////////////////////////////////////////////////////////////////////////////////////////

template<>
type type::get<detail::invalid_type>()
{
    return detail::get_invalid_type();
}

/////////////////////////////////////////////////////////////////////////////////////////

template<typename T>
type type::get(T&& object)
{
    using remove_ref = typename std::remove_reference<T>::type;
    return detail::type_from_instance<T, detail::has_get_type_func<T>::value && !std::is_pointer<remove_ref>::value>::get(std::forward<T>(object));
}
} // end namespace rttr


namespace std
{
    template <>
    struct hash<rttr::type>
    {
    public:
        size_t operator()(const rttr::type& info) const
        {
            return hash<rttr::type::type_id>()(info.get_id());
        }
    };
} // end namespace std

#define RTTR_CAT_IMPL(a, b) a##b
#define RTTR_CAT(a, b) RTTR_CAT_IMPL(a, b)

#define RTTR_REGISTRATION_STANDARD_TYPE_VARIANTS(T) rttr::type::get<T>();       \
                                                    rttr::type::get<T*>();      \
                                                    rttr::type::get<const T*>();
#endif // RTTR_TYPE_IMPL_H_
