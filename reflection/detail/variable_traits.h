/*************************************************************************
    > File Name: variable_traits.h
    > Author: hsz
    > Brief:
    > Created Time: 2024年03月23日 星期六 16时01分33秒
 ************************************************************************/

#ifndef __EULAR_REFLECTION_DETAIL_VARIABLE_TRAITS_H__
#define __EULAR_REFLECTION_DETAIL_VARIABLE_TRAITS_H__

#include <type_traits>

#include "std_type_traits.h"

namespace eular {
namespace detail {

template <typename T>
struct variable_type
{
    using raw_type = T;

    using type = remove_cv_t<remove_reference_t<T>>;
    static constexpr bool is_pointer = std::is_pointer<T>::value;
    static constexpr bool is_reference = std::is_reference<T>::value;
    static constexpr bool is_const = std::is_const<remove_reference_t<T>>::value;
    static constexpr bool is_member = false;
};

template <typename Class, typename T>
struct variable_type<T Class::*>
{
    using raw_type = T;

    using type = remove_cv_t<remove_reference_t<T>>;
    static constexpr bool is_pointer = std::is_pointer<T>::value;
    static constexpr bool is_reference = std::is_reference<T>::value;
    static constexpr bool is_const = std::is_const<remove_reference_t<T>>::value;
    static constexpr bool is_member = true;
};

} // namespace detail

template <typename T>
using variable_type_t = typename detail::variable_type<T>::type;

namespace internal {

template <typename T>
struct basic_variable_traits {
    using type = variable_type_t<T>;
    static constexpr bool is_member = detail::variable_type<T>::is_member;
};

}  // namespace internal




} // namespace eular

#endif // __EULAR_REFLECTION_DETAIL_VARIABLE_TRAITS_H__