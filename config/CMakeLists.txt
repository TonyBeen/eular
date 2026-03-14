cmake_minimum_required(VERSION 3.10)

# 设置项目名称和版本
project(libconfig VERSION 1.0)

# 指定C++标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED True)

add_subdirectory(3rd_party/libyaml)
add_subdirectory(3rd_party/c4core)
add_subdirectory(3rd_party/yyjson)