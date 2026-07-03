/*************************************************************************
    > File Name: function_traits.h
    > Author: hsz
    > Brief:
    > Created Time: Thu 15 Jun 2023 04:01:49 PM CST
 ************************************************************************/

#ifndef __FUNCTION_TRAITS_H__
#define __FUNCTION_TRAITS_H__

#include <type_traits>
#include <functional>
#include <tuple>

#include "std_type_traits.h"
#include "type_list.h"

#if !defined(__cpp_noexcept_function_type) || (__cpp_noexcept_function_type < 201510)
    #define NO_CXX17_NOEXCEPT_FUNC_TYPE
#endif

namespace eular {
namespace detail {

template<typename T>
struct is_function_ptr : std::integral_constant<bool,
                                                std::is_pointer<T>::value &&
                                                std::is_function<remove_pointer_t<remove_reference_t<T>>>::value>
{
};

struct helper
{
    void operator()(...);
};

template <typename T>
struct helper_composed: T, helper
{};

template <void (helper::*) (...)>
struct member_function_holder
{};

template <typename T, typename Ambiguous = member_function_holder<&helper::operator()> >
struct is_functor_impl : std::true_type
{};

template <typename T>
struct is_functor_impl<T, member_function_holder<&helper_composed<T>::operator()> > : std::false_type
{};

/**
 * @brief Returns true whether the given type T is a functor.
 *        i.e. func(...); That can be free function, lambdas or function objects.
 */
template <typename T>
struct is_functor : conditional_t<std::is_class<T>::value, is_functor_impl<T>, std::false_type>
{};

template<typename R, typename... Args>
struct is_functor<R (*)(Args...)> : std::true_type {};

template<typename R, typename... Args>
struct is_functor<R (&)(Args...)> : std::true_type {};

#ifndef NO_CXX17_NOEXCEPT_FUNC_TYPE
template<typename R, typename... Args>
struct is_functor<R (*)(Args...) noexcept> : std::true_type {};

template<typename R, typename... Args>
struct is_functor<R (&)(Args...) noexcept> : std::true_type {};
#endif

/////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////

template <typename T>
struct function_traits : function_traits< decltype(&T::operator()) > {};

template<typename R, typename... Args>
struct function_traits<R (Args...)>
{
    static constexpr size_t arg_count = sizeof...(Args);

    using return_type   = R;
    using arg_types     = type_list<Args...>;
};

template<typename R, typename... Args>
struct function_traits<R (*)(Args...)> : function_traits<R (Args...)> { };

template<typename R, typename... Args>
struct function_traits<R (&)(Args...)> : function_traits<R (Args...)> { };

template<typename R, typename C, typename... Args>
struct function_traits<R (C::*)(Args...)> : function_traits<R (Args...)> { using class_type = C; };

template<typename R, typename C, typename... Args>
struct function_traits<R (C::*)(Args...) const> : function_traits<R (Args...)> { using class_type = C; };

template<typename R, typename C, typename... Args>
struct function_traits<R (C::*)(Args...) volatile> : function_traits<R (Args...)> { using class_type = C; };

template<typename R, typename C, typename... Args>
struct function_traits<R (C::*)(Args...) const volatile> : function_traits<R (Args...)> {using class_type = C; };

#ifndef NO_CXX17_NOEXCEPT_FUNC_TYPE
template<typename R, typename... Args>
struct function_traits<R (*)(Args...) noexcept> : function_traits<R (Args...)> { };

template<typename R, typename... Args>
struct function_traits<R (&)(Args...) noexcept> : function_traits<R (Args...)> { };

template<typename R, typename C, typename... Args>
struct function_traits<R (C::*)(Args...) noexcept> : function_traits<R (Args...)> { using class_type = C; };

template<typename R, typename C, typename... Args>
struct function_traits<R (C::*)(Args...) const noexcept> : function_traits<R (Args...)> { using class_type = C; };

template<typename R, typename C, typename... Args>
struct function_traits<R (C::*)(Args...) volatile noexcept> : function_traits<R (Args...)> { using class_type = C; };

template<typename R, typename C, typename... Args>
struct function_traits<R (C::*)(Args...) const volatile noexcept> : function_traits<R (Args...)> {using class_type = C; };
#endif

template<typename T>
struct function_traits<std::function<T>> : function_traits<T> {};

// use it like e.g:
// param_types<F, 0>::type

template<typename F, size_t Index>
struct param_types
{
    using type = typename list_element<typename function_traits<F>::arg_types, Index>::type;
};

template<typename F, size_t Index>
using param_types_t = typename param_types<F, Index>::type;

template<typename F>
struct is_void_func : conditional_t<std::is_same<typename function_traits<F>::return_type, void>::value,
                                    std::true_type, std::false_type>
{
};

// returns an std::true_type, when the given type F is a function type; otherwise an std::false_type.
template<typename F>
using is_function = std::integral_constant<bool, std::is_member_function_pointer<F>::value ||
                                                 std::is_function<F>::value || is_functor<F>::value>;


} // namespace detail
} // end namespace eular

#endif // __FUNCTION_TRAITS_H__
