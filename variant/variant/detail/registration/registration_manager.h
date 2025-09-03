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

#ifndef RTTR_REGISTRATION_MANAGER_H_
#define RTTR_REGISTRATION_MANAGER_H_

#include "variant/detail/type/type_register.h"

#include <vector>
#include <memory>

namespace rttr
{
namespace detail
{

/*!
 * This class saves the registration of all possible items per module (*.DLL, *.so, ...)
 * and will undo the registration when the instance is destroyed.
 */
class registration_manager
{
    public:
        registration_manager()
        {
            type_register::register_reg_manager(this);
        }
        ~registration_manager()
        {
            unregister();
        }

        type_data* add_item(std::unique_ptr<type_data> obj)
        {
            auto reg_type = type_register::register_type(obj.get());
            const auto was_type_stored = (reg_type == obj.get());
            if (was_type_stored)
                m_type_data_list.push_back(std::move(obj)); // so we have to unregister it later

            return reg_type;
        }

        void set_disable_unregister()
        {
            m_should_unregister = false;
        }

        void unregister()
        {
            if (!m_should_unregister)
                return;

            for (auto& type : m_type_data_list)
                type_register::unregister_type(type.get());

            type_register::unregister_reg_manager(this);

            m_type_data_list.clear();
            m_should_unregister = false;
        }

        // no copy, no assign
        registration_manager(const registration_manager&) = delete;
        registration_manager& operator=(const registration_manager&) = delete;

    private:
        bool                                                    m_should_unregister = true;
        std::vector<std::unique_ptr<type_data>>                 m_type_data_list;
};

/////////////////////////////////////////////////////////////////////////////////////////


RTTR_INLINE registration_manager& get_registration_manager()
{
    static registration_manager obj;
    return obj;
}

} // end namespace detail
} // end namespace rttr

#endif // RTTR_REGISTRATION_MANAGER_H_
