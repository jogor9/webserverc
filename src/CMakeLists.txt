cmake_minimum_required(VERSION 3.27)
project(WEBSERVER
        VERSION 0.0.0.0
        LANGUAGES C)
set(CMAKE_C_FLAGS "-Wall -Wextra -pedantic -Oz")
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)
add_executable(webserver main.c)
