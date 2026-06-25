#include "reflection/type_info.h"

#include <type_traits>

namespace {

struct sample {
    const int c = 0;
    int value = 0;

    int method(double, const char*) const { return 0; }
};

int free_func(float, short) { return 0; }

} // namespace

static_assert(eular::detail::variable_type<const int&>::is_const, "const reference should be const-qualified");
static_assert(!eular::detail::variable_type<int&>::is_const, "non-const reference should not be const-qualified");
static_assert(eular::detail::variable_type<decltype(&sample::value)>::is_member, "member pointer should be member");

static_assert(eular::detail::is_function_ptr<decltype(&free_func)>::value, "free function pointer detection failed");

using member_traits = eular::detail::function_traits<decltype(&sample::method)>;
static_assert(std::is_same<member_traits::return_type, int>::value, "member return type mismatch");
static_assert(member_traits::arg_count == 2, "member arg count mismatch");
static_assert(std::is_same<eular::detail::param_types_t<decltype(&sample::method), 0>, double>::value,
              "first member arg type mismatch");

using free_traits = eular::detail::function_traits<decltype(free_func)>;
static_assert(std::is_same<free_traits::return_type, int>::value, "free return type mismatch");
static_assert(std::is_same<eular::detail::param_types_t<decltype(free_func), 1>, short>::value,
              "second free arg type mismatch");
