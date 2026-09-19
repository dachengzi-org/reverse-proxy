cmake_minimum_required(VERSION 3.10)
project(ReverseProxy)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# add executables
add_executable(proxy 
    src/main.cpp 
    src/ProxyServer.cpp
    src/picohttpparser.c
)