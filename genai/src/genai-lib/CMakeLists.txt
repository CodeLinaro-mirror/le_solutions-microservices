cmake_minimum_required(VERSION 3.8.2)
project(LLM_SERVICE LANGUAGES CXX)

add_subdirectory(src)
add_subdirectory(tests/c-test)
add_subdirectory(tests/cpp-test)
