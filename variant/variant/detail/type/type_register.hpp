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

#ifndef RTTR_TYPE_REGISTER_HPP_
#define RTTR_TYPE_REGISTER_HPP_

#include "variant/detail/type/type_register.h"
#include "variant/detail/type/type_data.h"
#include "variant/detail/type/type_register_p.h"
#include "variant/detail/type/type_string_utils.h"

#include "variant/detail/registration/registration_manager.h"

#include <set>

using namespace std;

namespace rttr
{
namespace detail
{

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE void type_register::register_reg_manager(registration_manager* manager)
{
    type_register_private::get_instance().register_reg_manager(manager);
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE void type_register::unregister_reg_manager(registration_manager* manager)
{
     type_register_private::get_instance().unregister_reg_manager(manager);
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE void type_register_private::register_custom_name(type& t, const std::string &custom_name)
{
    if (!t.is_valid())
        return;

    update_custom_name(custom_name.c_str(), t);

    // we have to make a copy of the list, because we also perform an insertion with 'update_custom_name'
    auto tmp_type_list = m_custom_name_to_id.value_data();
    for (auto& tt : tmp_type_list)
    {
        if (tt == t || !tt.is_template_instantiation())
            continue;

        update_custom_name(derive_template_instance_name(tt.m_type_data), tt);
    }
}

RTTR_INLINE void type_register::custom_name(type& t, const std::string &custom_name)
{
     type_register_private::get_instance().register_custom_name(t, custom_name);
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE type_register_private::type_register_private()
:   m_type_list({ type(get_invalid_type_data()) }),
    m_type_data_storage({ get_invalid_type_data() })
{
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE type_register_private::~type_register_private()
{
    // When this dtor is running, it means, that RTTR library will be unloaded
    // In order to avoid that the registration_manager instance's
    // are trying to deregister its content, although the RTTR library is already unloaded
    // every registration manager class holds a flag whether it should deregister itself or not
    // and we are settings this flag here
    for (auto& manager : m_registration_manager_list)
        manager->set_disable_unregister();
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE type_register_private& type_register_private::get_instance()
{
    static type_register_private obj;
    return obj;
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE void type_register_private::register_reg_manager(registration_manager* manager)
{
    m_registration_manager_list.insert(manager);
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE void type_register_private::unregister_reg_manager(registration_manager* manager)
{
    m_registration_manager_list.erase(manager);
}
/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE type_data* type_register::register_type(type_data* info)
{
    return type_register_private::get_instance().register_type(info);
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE void type_register::unregister_type(type_data* info)
{
    type_register_private::get_instance().unregister_type(info);
}

/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////
// free static functions

static bool rotate_char_when_whitespace_before(std::string& text, std::string::size_type& pos, char c)
{
    auto result = text.find(c, pos);
    if (result != std::string::npos && result > 0)
    {
        if (::isspace(text[result - 1]))
        {
            text[result - 1] = c;
            text[result] = ' ';
        }
        pos = result + 1;
        return true;
    }

    return false;
}

/////////////////////////////////////////////////////////////////////////////////////////

static void move_pointer_and_ref_to_type(std::string& type_name)
{
    std::string::size_type pos = 0;
    while(pos < type_name.length())
    {
        if (!rotate_char_when_whitespace_before(type_name, pos, '*') &&
            !rotate_char_when_whitespace_before(type_name, pos, '&') &&
            !rotate_char_when_whitespace_before(type_name, pos, ')'))
        {
            pos = std::string::npos;
        }
    }

    const auto non_whitespace = type_name.find_last_not_of(' ');
    type_name.resize(non_whitespace + 1);
}

/////////////////////////////////////////////////////////////////////////////////////////

static std::string derive_name_impl(const std::string& src_name, const std::string& raw_name,
                                    const std::string& custom_name)
{
    auto tmp_src_name = src_name;
    auto tmp_raw_name = raw_name;

    // We replace a custom registered name for a type for all derived types, e.g.
    // "std::basic_string<char>" => "std::string"
    // we want to use this also automatically for derived types like pointers, e.g.
    // "const std::basic_string<char>*" => "const std::string*"
    // therefore we have to replace the "raw_type" string
    remove_whitespaces(tmp_raw_name);
    remove_whitespaces(tmp_src_name);

    const auto start_pos = tmp_src_name.find(tmp_raw_name);
    const auto end_pos = start_pos + tmp_raw_name.length();
    if (start_pos == std::string::npos)
        return src_name; // nothing was found...

    // remember the two parts before and after the found "raw_name"
    const auto start_part = tmp_src_name.substr(0, start_pos);
    const auto end_part = tmp_src_name.substr(end_pos, tmp_src_name.length());

    tmp_src_name.replace(start_pos, tmp_raw_name.length(), custom_name);

    if (is_space_after(src_name, start_part))
        insert_space_after(tmp_src_name, start_part);

    if (is_space_before(src_name, end_part))
        insert_space_before(tmp_src_name, end_part);

    return tmp_src_name;
}

/////////////////////////////////////////////////////////////////////////////////////

template<typename T, typename I>
static bool remove_container_item(T& container, const I& item)
{
    bool result = false;
    container.erase(std::remove_if(container.begin(), container.end(),
                                   [&item, &result](I& item_)
                                   { return (item_== item) ? (result = true) : false; }),
                    container.end());

    return result;
}

/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////
//
// Here comes the implementation of the registration class 'type_register_private'
//
/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE type_data* type_register_private::register_name_if_neccessary(type_data* info)
{
    using namespace detail;

    auto ret = m_orig_name_to_id.find(info->type_name);
    if (ret != m_orig_name_to_id.end())
        return ret->m_type_data;

    std::lock_guard<std::mutex> lock(m_mutex);

    m_orig_name_to_id.insert(std::make_pair(info->type_name, type(info)));
    m_custom_name_to_id.insert(std::make_pair(info->type_name, type(info)));

    m_type_list.emplace_back(type(info));
    return nullptr;
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE type_data* type_register_private::register_type(type_data* info)
{
    // this will register the base types
    info->get_base_types();

    using namespace detail;

    if (auto t = register_name_if_neccessary(info))
        return t;

    info->raw_type_data  = !info->raw_type_data->is_valid ? info : info->raw_type_data;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_type_data_storage.push_back(info);
    }

    update_custom_name(derive_template_instance_name(info), type(info));
    return info;
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE void type_register_private::unregister_type(type_data* info)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    bool found_type_data = false;

    m_type_data_storage.erase(std::remove_if(m_type_data_storage.begin(), m_type_data_storage.end(),
                                             [&found_type_data, info](type_data* data)
                                             {
                                                 return (data == info) ? (found_type_data = true) : false;
                                             }),
                              m_type_data_storage.end()
                             );

    // we want to remove the name info, only when we found the correct type_data
    // it can be, that a duplicate type_data object will try to unregister itself
    if (found_type_data)
    {
        type obj_t(info);
        remove_container_item(m_type_list, obj_t);

        m_orig_name_to_id.erase(info->type_name);
        m_custom_name_to_id.erase(info->type_name);
    }
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE std::string type_register_private::derive_template_instance_name(type_data* info)
{
    auto& nested_types = info->get_class_data().m_nested_types;
    if (nested_types.empty()) // no template type
        return info->type_name;

    const auto has_custom_name = (info->type_name != info->type_name);
    if (has_custom_name)
        return info->type_name;

    const auto start_pos = info->type_name.find("<");
    const auto end_pos = info->type_name.rfind(">");

    if (start_pos == std::string::npos || end_pos == std::string::npos)
        return info->type_name;

    auto new_name = info->type_name.substr(0, start_pos);
    const auto end_part = info->type_name.substr(end_pos);
    auto index = nested_types.size();
    new_name += std::string("<");
    for (const auto& item : nested_types)
    {
        --index;
        const auto& custom_name = item.m_type_data->type_name;
        new_name += custom_name;
        if (index > 0)
            new_name += ",";
    }

    new_name += end_part;

    return new_name;
}

/////////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE void type_register_private::update_custom_name(std::string new_name, const type& t)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto& type_name = t.m_type_data->type_name;

    if (new_name != type_name)
    {
        m_custom_name_to_id.erase(type_name);

        type_name = std::move(new_name);
        m_custom_name_to_id.insert(std::make_pair(type_name, t));
    }
}

/////////////////////////////////////////////////////////////////////////////////////////

template<typename Container, typename Item>
static bool remove_item(Container& container, Item& item)
{
    using order = typename Container::value_type::order_by_id;
    auto itr = std::lower_bound(container.begin(), container.end(),
                                item, order());
    if (itr != container.end())
    {
        if ((*itr).m_data == item)
        {
            container.erase(itr);
            return true;
        }
    }
    return false;
}

/////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE std::vector<type_data*>& type_register_private::get_type_data_storage()
{
    return m_type_data_storage;
}

/////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE std::vector<type>& type_register_private::get_type_storage()
{
    return m_type_list;
}

/////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE flat_map<std::string, type>& type_register_private::get_orig_name_to_id()
{
    return m_orig_name_to_id;
}

/////////////////////////////////////////////////////////////////////////////////////

RTTR_INLINE flat_map<std::string, type, hash>& type_register_private::get_custom_name_to_id()
{
    return m_custom_name_to_id;
}

} // end namespace detail
} // end

#endif // RTTR_TYPE_REGISTER_HPP_