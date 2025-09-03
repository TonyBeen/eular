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

#ifndef RTTR_TYPE_REGISTER_P_H_
#define RTTR_TYPE_REGISTER_P_H_

#include "variant/detail/base/core_prerequisites.h"
#include "variant/detail/misc/flat_multimap.h"
#include "variant/detail/misc/flat_map.h"
#include "variant/detail/misc/utility.h"
#include "variant/variant.h"

#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <set>

namespace rttr
{
class type;

namespace detail
{
struct type_data;

/*!
 * This class contains all logic to register properties, methods etc.. for a specific type.
 * It is not part of the rttr API
 */
class type_register_private
{
public:

    /////////////////////////////////////////////////////////////////////////////////////
    RTTR_INLINE void register_reg_manager(registration_manager* manager);
    RTTR_INLINE void unregister_reg_manager(registration_manager* manager);

    /////////////////////////////////////////////////////////////////////////////////////

    RTTR_INLINE type_data* register_type(type_data* info);
    RTTR_INLINE void unregister_type(type_data* info);

    RTTR_INLINE void register_custom_name(type& t, const std::string &custom_name);

    /////////////////////////////////////////////////////////////////////////////////////

    RTTR_INLINE std::vector<type_data*>& get_type_data_storage();
    RTTR_INLINE std::vector<type>& get_type_storage();
    RTTR_INLINE flat_map<std::string, type>& get_orig_name_to_id();
    RTTR_INLINE flat_map<std::string, type, hash>& get_custom_name_to_id();

    static type_register_private& get_instance();

private:
    RTTR_INLINE type_register_private();
    RTTR_INLINE ~type_register_private();

    template<typename T, typename Data_Type = conditional_t<std::is_pointer<T>::value, T, std::unique_ptr<T>>>
    struct data_container
    {
        data_container(type::type_id id) : m_id(id) {}
        data_container(type::type_id id, Data_Type data) : m_id(id), m_data(std::move(data)) {}
        data_container(data_container<T, Data_Type>&& other) : m_id(other.m_id), m_data(std::move(other.m_data)) {}
        data_container<T, Data_Type>& operator = (data_container<T, Data_Type>&& other)
        {
            m_id = other.m_id;
            m_data = std::move(other.m_data);
            return *this;
        }

        struct order_by_id
        {
            bool operator () ( const data_container<T>& _left, const data_container<T>& _right )  const
            {
                return _left.m_id < _right.m_id;
            }
            bool operator () ( const type::type_id& _left, const data_container<T>& _right ) const
            {
                return _left < _right.m_id;
            }
            bool operator () ( const data_container<T>& _left, const type::type_id& _right ) const
            {
                return _left.m_id < _right;
            }

            bool operator () ( const data_container<T>& _left, const Data_Type& _right ) const
            {
                return _left.m_data < _right;
            }
        };

        type::type_id   m_id;
        Data_Type       m_data;
    };

    //! Returns true, when the name was already registered
    RTTR_INLINE type_data* register_name_if_neccessary(type_data* info);

    /*!
     * \brief This will create the derived name of a template instance, with all the custom names of a template parameter.
     * e.g.: `std::reference_wrapper<class std::basic_string<char,struct std::char_traits<char>,class std::allocator<char> > >` =>
     *       `std::reference_wrapper<class std::string>`
     *
     */
    static std::string derive_template_instance_name(type_data* info);

    /*!
     * Updates the custom name for the given type \p t with \p new_name
     */
    RTTR_INLINE void update_custom_name(std::string new_name, const type& t);

    /*! A helper class to register the registration managers.
     * This class is needed in order to avoid that the registration_manager instance's
     * are trying to deregister its content, although the RTTR library is already unloaded.
     * So every registration manager class holds a flag whether it should deregister itself or not.
     */
    std::set<registration_manager*>                             m_registration_manager_list;

    flat_map<std::string, type, hash>                           m_custom_name_to_id;
    flat_map<std::string, type>                                 m_orig_name_to_id;
    std::vector<type>                                           m_type_list;
    std::vector<type_data*>                                     m_type_data_storage;

    std::mutex                                                  m_mutex;
};

} // end namespace detail
} // end namespace rttr

#endif // RTTR_TYPE_REGISTER_P_H_
