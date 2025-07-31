/*************************************************************************
    > File Name: typecast.h
    > Author: hsz
    > Brief:
    > Created Time: Thu 12 Jan 2023 04:14:54 PM CST
 ************************************************************************/

#ifndef __CONFIG_TYPE_CAST_H__
#define __CONFIG_TYPE_CAST_H__

#include <string.h>
#include <stdlib.h>
#include <string>
#include <exception>

namespace eular {
template<typename T>
T type_cast(const char *src)
{
    throw std::bad_cast();
}

template<>
int type_cast<int>(const char *src)
{
    return std::stoi(src);
}

template<>
unsigned int type_cast<unsigned int>(const char *src)
{
    return static_cast<unsigned int>(std::stoul(src));
}

template<>
float type_cast<float>(const char *src)
{
    return std::stof(src);
}

template<>
double type_cast<double>(const char *src)
{
    return std::stod(src);
}

template<>
long type_cast<long>(const char *src)
{
    return std::stol(src);
}

template<>
unsigned long type_cast<unsigned long>(const char *src)
{
    return std::stoul(src);
}

template<>
long long type_cast<long long>(const char *src)
{
    return std::stoll(src);
}

template<>
unsigned long long type_cast<unsigned long long>(const char *src)
{
    return std::stoull(src);
}

template<>
bool type_cast<bool>(const char *src)
{
    bool flag = false;
    if (src && strcasecmp(src, "true") == 0) {
        flag = true;
    } else {
        flag = atoi(src) != 0;
    }

    return flag;
}

template<>
const char *type_cast<const char *>(const char *src)
{
    return src;
}

template<>
std::string type_cast<std::string>(const char *src)
{
    return std::string(src);
}

} // namespace eular

#endif // __CONFIG_TYPE_CAST_H__
