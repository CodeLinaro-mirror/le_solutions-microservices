cmake_minimum_required(VERSION 3.8.2)

project(solutions-microservices LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)

set(TOP_DIRECTORY ${CMAKE_CURRENT_LIST_DIR})

set(CMAKE_EXE_LINKER_FLAGS
    "${CMAKE_EXE_LINKER_FLAGS} -Wl,-no-undefined")
set(CMAKE_SHARED_LINKER_FLAGS
    "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-no-undefined")
set(CMAKE_STATIC_LINKER_FLAGS
    "${CMAKE_STATIC_LINKER_FLAGS} -Wl,-no-undefined")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-parameter")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unused-parameter")

include_directories(${SYSROOT_INCDIR})

link_directories(${SYSROOT_LIBDIR})
link_directories(${GST_PLUGINS_QTI_OSS_INSTALL_LIBDIR})
