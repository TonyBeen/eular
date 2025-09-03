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

#ifndef RTTR_VARIANT_HPP
#define RTTR_VARIANT_HPP

#include "variant/variant.h"

#include "variant/detail/variant/variant_data_policy.h"
#include "variant/argument.h"
#include "variant/detail/type/type_data.h"

#include <algorithm>
#include <limits>
#include <string>
#include <set>

namespace rttr
{

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE variant::variant(const variant& other)
:   m_policy(other.m_policy)
{
    m_policy(detail::variant_policy_operation::CLONE, other.m_data, m_data);
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE variant::variant(variant&& other)
:   m_policy(other.m_policy)
{
    other.m_policy(detail::variant_policy_operation::SWAP, other.m_data, m_data);
    other.m_policy = &detail::variant_data_policy_empty::invoke;
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE void variant::swap(variant& other)
{
    if (this == &other)
        return;

    const bool is_this_valid = is_valid();
    const bool is_other_valid = other.is_valid();

    if (!is_this_valid && !is_other_valid)
        return;

    if (is_this_valid && is_other_valid)
    {
        detail::variant_data tmp_data;
        detail::variant_policy_func tmp_policy_func = other.m_policy;
        other.m_policy(detail::variant_policy_operation::SWAP, other.m_data, tmp_data);

        m_policy(detail::variant_policy_operation::SWAP, m_data, other.m_data);
        other.m_policy = m_policy;

        tmp_policy_func(detail::variant_policy_operation::SWAP, tmp_data, m_data);
        m_policy = tmp_policy_func;
    }
    else
    {
        detail::variant_data& full_data = is_this_valid ? m_data : other.m_data;
        detail::variant_data& empty_data = is_this_valid ? other.m_data : m_data;
        detail::variant_policy_func full_policy_func = is_this_valid ? m_policy : other.m_policy;

        full_policy_func(detail::variant_policy_operation::SWAP, full_data, empty_data);

        std::swap(m_policy, other.m_policy);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE variant& variant::operator=(const variant& other)
{
    if (this == &other)
        return *this;

    m_policy(detail::variant_policy_operation::DESTROY, m_data, detail::argument_wrapper());
    other.m_policy(detail::variant_policy_operation::CLONE, other.m_data, m_data);
    m_policy = other.m_policy;

    return *this;
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE variant& variant::operator=(variant&& other)
{
    m_policy(detail::variant_policy_operation::DESTROY, m_data, detail::argument_wrapper());
    other.m_policy(detail::variant_policy_operation::SWAP, other.m_data, m_data);
    m_policy = other.m_policy;
    other.m_policy = &detail::variant_data_policy_empty::invoke;

    return *this;
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE void variant::clear()
{
    m_policy(detail::variant_policy_operation::DESTROY, m_data, detail::argument_wrapper());
    m_policy = &detail::variant_data_policy_empty::invoke;
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE bool variant::is_valid() const
{
    return m_policy(detail::variant_policy_operation::IS_VALID, m_data, detail::argument_wrapper());
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE variant::operator bool() const
{
    return m_policy(detail::variant_policy_operation::IS_VALID, m_data, detail::argument_wrapper());
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE type variant::get_type() const
{
    type src_type = detail::get_invalid_type();
    m_policy(detail::variant_policy_operation::GET_TYPE, m_data, src_type);
    return src_type;
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE variant variant::extract_wrapped_value() const
{
    variant var;
    m_policy(detail::variant_policy_operation::EXTRACT_WRAPPED_VALUE, m_data, var);
    return var;
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE variant variant::create_wrapped_value(const type& wrapped_type) const
{
    variant var;
    m_policy(detail::variant_policy_operation::CREATE_WRAPPED_VALUE, m_data, std::tie(var, wrapped_type));
    return var;
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE bool variant::can_convert(const type& target_type) const
{
    if (!is_valid())
        return false;

    type source_type = get_type();
    source_type = (source_type.is_wrapper() && !target_type.is_wrapper()) ? source_type.get_wrapped_type() : source_type;

    if (source_type == target_type)
        return true;

    if (!source_type.is_wrapper() && target_type.is_wrapper())
    {
        if (target_type.get_wrapped_type() == source_type && target_type.m_type_data->create_wrapper)
            return true;
    }

    if (target_type == type::get<std::nullptr_t>() && is_nullptr())
        return true;

    const bool source_is_arithmetic = source_type.is_arithmetic();
    const bool target_is_arithmetic = target_type.is_arithmetic();
    const bool target_is_enumeration = target_type.is_enumeration();
    const type string_type = type::get<std::string>();

    return ((source_is_arithmetic && target_is_arithmetic) ||
            (source_is_arithmetic && target_type == string_type) ||
            (source_type == string_type && target_is_arithmetic) ||
            (source_type.is_enumeration() && target_is_arithmetic) ||
            (source_is_arithmetic && target_is_enumeration) ||
            (source_type == string_type && target_is_enumeration));
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE bool variant::convert(const type& target_type, variant& target_var) const
{
    if (!is_valid())
        return false;

    bool ok = false;

    const type source_type = get_type();
    const bool source_is_arithmetic = source_type.is_arithmetic();
    const bool target_is_arithmetic = target_type.is_arithmetic();
    const type string_type = type::get<std::string>();
    if (target_type == source_type)
    {
        target_var = *this;
        return true; // the current variant is already the target type, we don't need to do anything
    }
    else if (!source_type.is_wrapper() && target_type.is_wrapper() &&
             target_type.get_wrapped_type() == source_type)
    {
        target_var = create_wrapped_value(target_type);
        ok = target_var.is_valid();
    }
    else if (source_type.is_wrapper() && !target_type.is_wrapper())
    {
        variant var = extract_wrapped_value();
        ok = var.convert(target_type);
        target_var = var;
    }
    else if ((source_is_arithmetic && target_is_arithmetic) ||
            (source_is_arithmetic && target_type == string_type) ||
            (source_type == string_type && target_is_arithmetic) ||
            (source_type.is_enumeration() && target_is_arithmetic) ||
            (source_type.is_enumeration() && target_type == string_type))
    {
        if (target_type == type::get<bool>())
        {
            bool value;
            if ((ok = try_basic_type_conversion(value)) == true)
                target_var = value;
        }
        else if (target_type == type::get<char>())
        {
            char value;
            if ((ok = try_basic_type_conversion(value)) == true)
                target_var = value;
        }
        else if (target_type == type::get<int8_t>())
        {
            int8_t value;
            if ((ok = try_basic_type_conversion(value)) == true)
                target_var = value;
        }
        else if (target_type == type::get<int16_t>())
        {
            int16_t value;
            if ((ok = try_basic_type_conversion(value)) == true)
                target_var = value;
        }
        else if (target_type == type::get<int32_t>())
        {
            int32_t value;
            if ((ok = try_basic_type_conversion(value)) == true)
                target_var = value;
        }
        else if (target_type == type::get<int64_t>())
        {
            int64_t value;
            if ((ok = try_basic_type_conversion(value)) == true)
                target_var = value;
        }
        else if (target_type == type::get<uint8_t>())
        {
            uint8_t value;
            if ((ok = try_basic_type_conversion(value)) == true)
                target_var = value;
        }
        else if (target_type == type::get<uint16_t>())
        {
            uint16_t value;
            if ((ok = try_basic_type_conversion(value)) == true)
                target_var = value;
        }
        else if (target_type == type::get<uint32_t>())
        {
            uint32_t value;
            if ((ok = try_basic_type_conversion(value)) == true)
                target_var = value;
        }
        else if (target_type == type::get<uint64_t>())
        {
            uint64_t value;
            if ((ok = try_basic_type_conversion(value)) == true)
                target_var = value;
        }
        else if (target_type == type::get<float>())
        {
            float value;
            if ((ok = try_basic_type_conversion(value)) == true)
                target_var = value;
        }
        else if (target_type == type::get<double>())
        {
            double value;
            if ((ok = try_basic_type_conversion(value)) == true)
                target_var = value;
        }
        else if (target_type == string_type)
        {
            std::string value;
            if ((ok = try_basic_type_conversion(value)) == true)
                target_var = std::move(value);
        }
    }
    else if ((source_is_arithmetic || source_type == string_type)
             && target_type.is_enumeration())
    {
        variant var = target_type;
        auto wrapper = std::ref(var);
        if ((ok = try_basic_type_conversion(wrapper)) == true)
            target_var = std::move(var);
    }
    else
    {
        if (target_type == type::get<std::nullptr_t>() && is_nullptr())
        {
            target_var = nullptr;
            ok = true;
        }
        else if (source_type.is_pointer() &&
                (source_type.m_type_data->get_pointer_dimension == 1 && target_type.m_type_data->get_pointer_dimension == 1))
        {
            void* raw_ptr = get_raw_ptr();
            auto& src_raw_type = source_type.m_type_data->raw_type_data;
            auto& tgt_raw_type = target_type.m_type_data->raw_type_data;
            if (src_raw_type == tgt_raw_type)
            {
                // although we forward a void* to create a variant,
                // it will create a variant for the specific class type
                target_var = target_type.create_variant(raw_ptr);
                if (target_var.is_valid())
                    ok = true;
            }
        }
    }

    return ok;
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE bool variant::convert(const type& target_type)
{
    return convert(target_type, *this);
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE bool variant::to_bool() const
{
    return convert<bool>(nullptr);
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE int variant::to_int(bool *ok) const
{
    return convert<int>(ok);
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE std::string variant::to_string(bool *ok) const
{
    return convert<std::string>(ok);
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE float variant::to_float(bool* ok) const
{
    return convert<float>(ok);
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE double variant::to_double(bool* ok) const
{
    return convert<double>(ok);
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE int8_t variant::to_int8(bool *ok) const
{
    return convert<int8_t>(ok);
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE int16_t variant::to_int16(bool *ok) const
{
    return convert<int16_t>(ok);
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE int32_t variant::to_int32(bool *ok) const
{
    return convert<int32_t>(ok);
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE int64_t variant::to_int64(bool *ok) const
{
    return convert<int64_t>(ok);
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE uint8_t variant::to_uint8(bool *ok) const
{
    return convert<uint8_t>(ok);
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE uint16_t variant::to_uint16(bool *ok) const
{
    return convert<uint16_t>(ok);
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE uint32_t variant::to_uint32(bool *ok) const
{
    return convert<uint32_t>(ok);
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE uint64_t variant::to_uint64(bool *ok) const
{
    return convert<uint64_t>(ok);
}

} // end namespace rttr

#endif // RTTR_VARIANT_HPP