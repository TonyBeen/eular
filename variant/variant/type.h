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

#ifndef __RTTR_TYPE_H__
#define __RTTR_TYPE_H__

#include <type_traits>
#include <vector>
#include <string>
#include <memory>
#include <cstdint>

namespace rttr
{
class variant;
class type;
class instance;
class argument;

namespace detail
{
struct derived_info;
struct base_class_info;
class type_register;
class type_register_private;

static type get_invalid_type();
struct invalid_type{};
struct type_data;
type create_type(type_data*);

template<typename T>
std::unique_ptr<type_data> make_type_data();

template<typename T, typename Tp, typename Converter>
struct variant_data_base_policy;

} // end namespace detail

/*!
 * The \ref type class holds the type information for any arbitrary object.
 *
 * Every class or primitive data type can have an unique type object.
 * With the help of this object you can compare unknown types for equality at runtime or introspect the type
 * for its \ref property "properties", \ref method "methods", \ref enumeration "enumerations",
 * \ref constructor "constructors" and \ref destructor "destructor".
 *
 * Retrieve %type
 * ------------------
 * A type object **cannot** be created. It is only possible to retrieve a type object via three static template member functions:
 *
 * ### type::get<T>() ###
 *
 * This function just expects one template argument. Use it to check against a known type.
 *
 * \code{.cpp}
 *      type::get<int>() == type::get<int>();  // yields to true
 *      type::get<int>() == type::get<bool>(); // yields to false
 * \endcode
 *
 * ### type::get_by_name(string_view) ###
 *
 * This function just expects the name of the type. This is useful when you know only the name of the type and cannot include the type itself into the source code.
 *
 * \code{.cpp}
 *      type::get_by_name("int")  == type::get<int>(); // yields to true
 *      type::get_by_name("bool") == type::get<int>(); // yields to false
 *      type::get_by_name("MyNameSpace::MyStruct") == type::get<MyNameSpace::MyStruct>();  // yields to true
 * \endcode
 *
 * \remark Before using the function \ref type::get_by_name(), you have to use one time the function via \ref type::get<T>(), otherwise the type is not registered in the type system.
 *
 * ### type::get<T>(T&& obj) ###
 *
 * This function takes a universal reference and returns from every given object the corresponding type object.
 *
 * \code{.cpp}
 *      int int_obj;
 *      int* int_obj_ptr = &int_obj;
 *      const int* c_int_obj_ptr = int_obj_ptr;
 *
 *      type::get<int>()         == type::get(int_obj);        // yields to true
 *      type::get<int*>()        == type::get(int_obj_ptr);    // yields to true
 *      type::get<const int*>()  == type::get(c_int_obj_ptr);  // yields to true
 * \endcode
 *
 * When this function is called for a glvalue expression whose type is a polymorphic class type,
 * then the result refers to a \ref type object representing the type of the most derived object.
 *
 * \code{.cpp}
 *      struct Base { RTTR_ENABLE() };
 *      struct Derived : Base { RTTR_ENABLE(Base) };
 *      //...
 *      Derived d;
 *      Base& base = d;
 *      type::get<Derived>()   == type::get(base);      // yields to true
 *      type::get<Base>()      == type::get(base);      // yields to false
 *
 *      // remark, when called with pointers:
 *      Base* base_ptr = &d;
 *      type::get<Derived>()   == type::get(base_ptr);  // yields to false
 *      type::get<Base*>()     == type::get(base_ptr);  // yields to true
 * \endcode
 *
 * \remark If the type of the expression is a cv-qualified type, the result of the rttr::type::get expression refers to a rttr::type object representing the cv-unqualified type.
 *
 * \code{.cpp}
 *      class D { ... };
 *      D d1;
 *      const D d2;
 *      type::get(d1)  == type::get(d2);         // yields true
 *      type::get<D>() == type::get<const D>();  // yields true
 *      type::get<D>() == type::get(d2);         // yields true
 *      type::get<D>() == type::get<const D&>(); // yields true
 *      type::get<D>() == type::get<const D*>(); // yields false
 * \endcode
 * Any `top level` cv-qualifier of the given type `T` will be removed.
 *
 *
 * Copying and Assignment
 * ----------------------
 * A \ref type object is lightweight and can be copied by value. However, each copy will refer to the same underlying type.
 *
 */
class type
{
    public:
        typedef uintptr_t type_id;

        /*!
         * \brief Assigns a type to another one.
         *
         */
        type(const type& other);

        /*!
         * \brief Assigns a type to another one.
         *
         * \return A type object.
         */
        type& operator=(const type& other);

        /*!
         * \brief Comparison operator for sorting the type data according to some internal criterion.
         *
         * \return True if this type is less than the \a other.
         */
        bool operator<(const type& other) const;

        /*!
         * \brief Comparison operator for sorting the type data according to some internal criterion.
         *
         * \return True if this type is greater than the \a other.
         */
        bool operator>(const type& other) const;

        /*!
         * \brief Comparison operator for sorting the type data according to some internal criterion.
         *
         * \return True if this type is greater than or equal to \a other.
         */
        bool operator>=(const type& other) const;

        /*!
         * \brief Comparison operator for sorting the type data according to some internal criterion.
         *
         * \return True if this type is less than or equal to \a other.
         */
        bool operator<=(const type& other) const;

        /*!
         * \brief Compares this type with the \a other type and returns true
         *        if both describe the same type, otherwise returns false.
         *
         * \return True if both type are equal, otherwise false.
         */
        bool operator==(const type& other) const;

        /*!
         * \brief Compares this type with the \a other type and returns true
         *        if both describe different types, otherwise returns false.
         *
         * \return True if both type are \b not equal, otherwise false.
         */
        bool operator!=(const type& other) const;

        /*!
         * \brief Returns the id of this type.
         *
         * \note This id is unique at process runtime,
         *       but the id can be changed every time the process is executed.
         *
         * \return The type id.
         */
        type_id get_id() const;

        /*!
         * \brief Returns the unique and human-readable name of the type.
         *
         * \remark The content of this string is compiler depended.
         *
         * \return The type name.
         */
        std::string get_name() const;

        /*!
         * \brief Returns true if this type is valid, that means the type holds valid data to a type.
         *
         * \return True if this type is valid, otherwise false.
         */
        bool is_valid() const;

        /*!
         * \brief Convenience function to check if this \ref type is valid or not.
         *
         * \return True if this \ref type is valid, otherwise false.
         */
         explicit operator bool() const;

        /*!
         * \brief Returns a type object which represent the raw type.
         *        A raw type, is a type type without any qualifiers (const and volatile) nor any pointer.
         *
         * \remark When the current type is already the raw type, it will return an copy from itself.
         *
         * \return The corresponding raw type object.
         */
        type get_raw_type() const;

        /*!
         * \brief Returns a type object which represent the wrapped type.
         *        A wrapper type is a class which encapsulate an instance of another type.
         *        This encapsulate type is also called *wrapped type*.
         *
         * See following example code:
         * \code{.cpp}
         *   type wrapped_type = type::get<std::shared_ptr<int>>().get_wrapped_type();
         *   wrapped_type == type::get<int*>(); // yields to true
         *
         *   wrapped_type = type::get<std::reference_wrapper<int>>().get_wrapped_type();
         *   wrapped_type == type::get<int>(); // yields to true
         * \endcode
         *
         * \remark When the current type is not a wrapper type, this function will return an \ref type::is_valid "invalid type".
         *
         * \see \ref wrapper_mapper "wrapper_mapper<T>"
         *
         * \return The type object of the wrapped type.
         */
        type get_wrapped_type() const;

        /*!
         * \brief Returns a type object for the given template type \a T.
         *
         * \return type for the template type \a T.
         */
        template<typename T>
        static type get();

        /*!
         * \brief Returns a type object for the given instance \a object.
         *
         * \remark If the type of the expression is a cv-qualified type, the result of the type::get() expression refers to a
         *         type object representing the cv-unqualified type.
         *         When type::get() is applied to a glvalue expression whose type is a polymorphic class type,
         *         the result refers to a type object representing the type of the most derived object.
         *
         * \return type for an \a object of type \a T.
         */
        template<typename T>
        static type get(T&& object);

        /*!
         * \brief Returns the type object with the given name \p name.
         *
         * \remark The search for the type is case sensitive. White spaces will be ignored.
         *         The name of the type corresponds to the name which was used during \ref RTTR_REGISTRATION.
         *         Only after the registration process was executed, then the type can be retrieved with this function.
         *         Otherwise and invalid type will be returned.
         *
         * \return \ref type object with the name \p name.
         */
        static type get_by_name(const std::string &name);

        /*!
         * \brief Returns the size in bytes of the object representation of the current type (i.e. `sizeof(T)`).
         *
         * \return The size of the type in bytes.
         */
        std::size_t get_sizeof() const;

        /*!
         * \brief Returns true whether the given type is class; that is not an atomic type or a method.
         *
         * \return True if the type is a class, otherwise false.
         */
        bool is_class() const;

         /*!
         * \brief Returns true whether the given type is an instantiation of a class template.
         *
         * See following example code:
         * \code{.cpp}
         * template<typename T>
         * struct foo { }; // class template
         *
         * struct bar { }; // NO class template
         *
         * type::get<foo<int>>().is_template_instantiation(); // yield to 'true'
         * type::get<bar>().is_template_instantiation();      // yield to 'false'
         * \endcode
         *
         * \return `true` if the type is a class template, otherwise `false`.
         *
         * \see get_template_arguments()
         */
        bool is_template_instantiation() const;

        /*!
         * \brief Returns true whether the given type represents an enumeration.
         *
         * \return True if the type is an enumeration, otherwise false.
         */
        bool is_enumeration() const;

        /*!
         * \brief Returns true whether the given type represents a wrapper type.
         *        A wrapper type is a class which encapsulate an instance of another type.
         *        RTTR recognize automatically following wrapper types:
         *        - \p `std::shared_ptr<T>`
         *        - \p `std::reference_wrapper<T>`
         *        - \p `std::weak_ptr<T>`
         *        - \p `std::unique_ptr<T>`
         *
         *        In order to work with custom wrapper types, its required to specialize the class \ref wrapper_mapper "wrapper_mapper<T>"
         *        and implement a getter function to retrieve the encapsulate type.
         *
         * \see \ref wrapper_mapper "wrapper_mapper<T>"
         *
         * \return True if the type is an wrapper, otherwise false.
         *
         */
        bool is_wrapper() const;

        /*!
         * \brief Returns `true` whether the given type represents an array.
         *        An array is always also a sequential container.
         *        The check will return `true` only for raw C-Style arrays:
         * \code{.cpp}
         *
         *  type::get<int[10]>().is_array();            // true
         *  type::get<int>().is_array();                // false
         *  type::get<std::array<int,10>>().is_array(); // false
         * \endcode
         *
         * \return `true` if the type is an array, otherwise `false`.
         *
         * \see is_sequential_container()
         */
        bool is_array() const;

        /*!
         * \brief Returns true whether the given type represents an
         *        <a target="_blank" href=https://en.wikipedia.org/wiki/Associative_containers>associative container</a>.
         *
         * \return True if the type is an associative container, otherwise false.
         *
         * \see \ref associative_container_mapper "associative_container_mapper<T>"
         */
        bool is_associative_container() const;

        /*!
         * \brief Returns true whether the given type represents an
         *        <a target="_blank" href=https://en.wikipedia.org/wiki/Sequence_container_(C%2B%2B)>sequence container</a>.
         *
         * \return True if the type is an sequential container, otherwise false.
         *
         * \see \ref sequential_container_mapper "sequential_container_mapper<T>"
         */
        bool is_sequential_container() const;

        /*!
         * \brief Returns true whether the given type represents a pointer.
         *        e.g. `int*`, or `bool*`
         *
         * \return True if the type is a pointer, otherwise false.
         */
        bool is_pointer() const;

        /*!
         * \brief Returns true whether the given type represents an arithmetic type.
         *        An arithmetic type is a integral type or a floating point type.
         *        e.g. `bool`, `int`, `float`, etc...
         *
         * \return True if the type is a arithmetic type, otherwise false.
         */
        bool is_arithmetic() const;


    private:

        /*!
         * Constructs an empty and invalid type object.
         */
        type();

        /*!
         * \brief Constructs a valid type object.
         *
         * \param id The unique id of the data type.
         */
        explicit type(detail::type_data* data);

        //! Creates a variant from the given argument data.
        variant create_variant(const argument& data) const;

        friend class variant;
        friend class instance;
        friend class detail::type_register;
        friend class detail::type_register_private;

        friend type detail::create_type(detail::type_data*);

        template<typename T>
        friend std::unique_ptr<detail::type_data> detail::make_type_data();

        template<typename T, typename Tp, typename Converter>
        friend struct detail::variant_data_base_policy;

    private:
        detail::type_data* m_type_data;
};

} // end namespace rttr

#include "variant/detail/type/type_impl.h"
#include "variant/type.hpp"

#endif // __RTTR_TYPE_H__
