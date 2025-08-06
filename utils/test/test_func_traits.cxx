/*************************************************************************
    > File Name: test_func_traits.cc
    > Author: hsz
    > Brief:
    > Created Time: Thu 15 Jun 2023 04:05:21 PM CST
 ************************************************************************/

// #include <utils/function_traits.h>
// #include <iostream>
// #include <iomanip>

// using namespace std;

// #define WIDTH 80

// // 普通函数
// void function_return_void_with_no_arg()
// {

// }

// void function_return_void_with_no_arg_noexcept() noexcept
// {

// }

// void function_return_void_with_arg(int a)
// {
    
// }

// int function_return_int_with_no_arg()
// {
//     return 0;
// }

// int function_return_int_with_arg(int a)
// {
//     return a;
// }

// // lambda
// auto lambda_return_void_with_no_arg = []() -> void {

// };

// auto lambda_return_void_with_no_arg_noexcept = []() noexcept -> void {

// };

// auto lambda_return_void_with_arg = [](int a) -> void {

// };

// auto lambda_return_int_with_no_arg = []() -> int {
//     return 0;
// };

// auto lambda_return_int_with_arg = [](int a) -> int {
//     return a;
// };

// // class function

// class Func {
// public:
//     void function_return_void_with_no_arg()
//     {

//     }

//     void function_return_void_with_no_arg_noexcept() noexcept
//     {

//     }

//     void function_return_void_with_no_arg_const() const
//     {

//     }

//     void function_return_void_with_no_arg_const_volatile() const volatile
//     {

//     }

//     void function_return_void_with_arg(int a)
//     {
        
//     }

//     int function_return_int_with_no_arg()
//     {
//         return 0;
//     }

//     int function_return_int_with_arg(int a)
//     {
//         return a;
//     }

//     void operator()()
//     {

//     }
// };

// void test_function()
// {
//     std::cout << "----------------------------------------------------------------------------\n";
//     // normal function
//     std::cout << std::left << std::setw(WIDTH) <<
//         "function_return_void_with_no_arg is function" << " ? " <<
//         eular::is_function<decltype(function_return_void_with_no_arg)>::value << std::endl;

//     // std::cout << "type name" << typeid(decltype(function_return_void_with_no_arg)).name() << std::endl; // 不知道为啥是这个nameFvvE

//     std::cout << std::left << std::setw(WIDTH) <<
//         "function_return_void_with_no_arg is function" << " ? " <<
//         eular::is_function<decltype(function_return_void_with_no_arg_noexcept)>::value << std::endl;

//     std::cout << std::left << std::setw(WIDTH) <<
//         "function_return_void_with_arg is function" << " ? " <<
//         eular::is_function<decltype(function_return_void_with_arg)>::value << std::endl;

//     std::cout << std::left << std::setw(WIDTH) <<
//         "function_return_int_with_no_arg is function" << " ? " <<
//         eular::is_function<decltype(function_return_int_with_no_arg)>::value << std::endl;

//     std::cout << std::left << std::setw(WIDTH) <<
//         "function_return_int_with_arg is function" << " ? " <<
//         eular::is_function<decltype(function_return_int_with_arg)>::value << std::endl;
// }

// void test_lambda()
// {
//     std::cout << "----------------------------------------------------------------------------\n";
//     // lambda
//     std::cout << std::left << std::setw(WIDTH) <<
//         "lambda_return_void_with_no_arg is function" << " ? " <<
//         eular::is_function<decltype(lambda_return_void_with_no_arg)>() << std::endl;

//     std::cout << std::left << std::setw(WIDTH) <<
//         "lambda_return_void_with_no_arg_noexcept is function" << " ? " <<
//         eular::is_function<decltype(lambda_return_void_with_no_arg_noexcept)>() << std::endl;

//     std::cout << std::left << std::setw(WIDTH) <<
//         "lambda_return_void_with_arg is function" << " ? " <<
//         eular::is_function<decltype(lambda_return_void_with_arg)>() << std::endl;

//     std::cout << std::left << std::setw(WIDTH) <<
//         "lambda_return_int_with_no_arg is function" << " ? " <<
//         eular::is_function<decltype(lambda_return_int_with_no_arg)>() << std::endl;

//     std::cout << std::left << std::setw(WIDTH) <<
//         "lambda_return_int_with_arg is function" << " ? " <<
//         eular::is_function<decltype(lambda_return_int_with_arg)>() << std::endl;
// }

// void test_class_function()
// {
//     std::cout << "----------------------------------------------------------------------------\n";

//     // class function
//     std::cout << std::left << std::setw(WIDTH) <<
//         "&Func::function_return_int_with_arg is function" << " ? " <<
//         eular::is_function<decltype(&Func::function_return_int_with_arg)>() << std::endl;
    
//     std::cout << std::left << std::setw(WIDTH) <<
//         "&Func::function_return_int_with_no_arg is function" << " ? " <<
//         eular::is_function<decltype(&Func::function_return_int_with_no_arg)>() << std::endl;
    
//     std::cout << std::left << std::setw(WIDTH) <<
//         "&Func::function_return_void_with_arg is function" << " ? " <<
//         eular::is_function<decltype(&Func::function_return_void_with_arg)>() << std::endl;
    
//     std::cout << std::left << std::setw(WIDTH) <<
//         "&Func::function_return_void_with_no_arg is function" << " ? " <<
//         eular::is_function<decltype(&Func::function_return_void_with_no_arg)>() << std::endl;
    
//     std::cout << std::left << std::setw(WIDTH) <<
//         "&Func::function_return_void_with_no_arg_const is function" << " ? " <<
//         eular::is_function<decltype(&Func::function_return_void_with_no_arg_const)>() << std::endl;
    
//     std::cout << std::left << std::setw(WIDTH) <<
//         "&Func::function_return_void_with_no_arg_const_volatile is function" << " ? " <<
//         eular::is_function<decltype(&Func::function_return_void_with_no_arg_const_volatile)>() << std::endl;
    
//     std::cout << std::left << std::setw(WIDTH) <<
//         "&Func::function_return_void_with_no_arg_noexcept is function" << " ? " <<
//         eular::is_function<decltype(&Func::function_return_void_with_no_arg_noexcept)>() << std::endl;
    
//     std::cout << std::left << std::setw(WIDTH) <<
//         "&Func::operator() is function" << " ? " <<
//         eular::is_function<decltype(&Func::operator())>() << std::endl;
// }

// int main(int argc, char **argv)
// {
//     test_function();
//     test_class_function();
//     test_lambda();

//     return 0;
// }
