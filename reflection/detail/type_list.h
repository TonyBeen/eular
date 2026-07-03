/*************************************************************************
    > File Name: type_list.h
    > Author: hsz
    > Brief:
    > Created Time: 2024年03月17日 星期日 17时13分32秒
 ************************************************************************/

#ifndef __REFLECTION_DETAIL_TYPE_LIST_H__
#define __REFLECTION_DETAIL_TYPE_LIST_H__

#include <stddef.h>
#include <type_traits>

#include "std_version.h"

namespace eular {
namespace detail {

template <typename... Ts>
struct type_list
{
    using self_type = type_list<Ts...>;
    static constexpr size_t size = sizeof...(Ts);
};

template <typename, size_t>
struct list_element;

template <template <typename...> class ListType, typename T, typename... Ts, size_t N>
struct list_element<ListType<T, Ts...>, N> : list_element<ListType<Ts...>, N - 1>
{
};

template <template <typename...> class ListType, typename T, typename... Ts>
struct list_element<ListType<T, Ts...>, 0>
{
    using type = T;
};

template <typename>
struct list_size;

template <template <typename...> class ListType, typename... Ts>
struct list_size<ListType<Ts...>>
{
    static constexpr size_t value = sizeof...(Ts);
};

template <typename>
struct list_is_empty;

template <template <typename...> class ListType, typename... Ts>
struct list_is_empty<ListType<Ts...>>
{
    static constexpr bool value = sizeof...(Ts) == 0;
};

template <typename>
struct list_head;

template <template <typename...> class ListType, typename T, typename... Ts>
struct list_head<ListType<T, Ts...>> {
    using type = T;
};

template <typename>
struct list_tail;

template <template <typename...> class ListType, typename T, typename... Ts>
struct list_tail<ListType<T, Ts...>> {
    using type = ListType<Ts...>;
};

template <typename List, typename T>
struct list_add_to_first;

template <template <typename...> class ListType, typename... Ts, typename T>
struct list_add_to_first<ListType<Ts...>, T> {
    using type = ListType<T, Ts...>;
};

// 空的 type_list
template <typename TypeList1, typename TypeList2>
struct is_type_list_same {
    static constexpr bool value = std::is_same<TypeList1, TypeList2>::value;
};

// 非空的 type_list
template <template <typename...> class ListType, typename T, typename... Ts, typename T2, typename... Ts2>
struct is_type_list_same<ListType<T, Ts...>, ListType<T2, Ts2...>> {
    static constexpr bool value = std::is_same<T, T2>::value && is_type_list_same<ListType<Ts...>, ListType<Ts2...>>::value;
};

} // namespace detail

template <typename List, size_t N>
using list_element_t = typename detail::list_element<List, N>::type;

template <typename List>
using list_head_t = typename detail::list_head<List>::type;

template <typename List>
using list_tail_t = typename detail::list_tail<List>::type;

template <typename List, typename T>
using list_add_to_first_t = typename detail::list_add_to_first<List, T>::type;

#if CPP_VERSION >= CPP_VERSION_17

template <typename List>
constexpr size_t list_size_v = detail::list_size<List>::value;

template <typename List>
constexpr bool list_is_empty_v = detail::list_is_empty<List>::value;

#endif

} // namespace eular

#endif // __REFLECTION_DETAIL_TYPE_LIST_H__
