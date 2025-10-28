cmake_minimum_required(VERSION 3.8.2)
project(LLM_SERVICE LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

# Include directories
include_directories(${SYSROOT_INCDIR})

# Link directories
link_directories(${SYSROOT_LIBDIR})

# Find jsoncpp package
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

# Common compiler flags.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unused-parameter")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-parameter")

# Precompiler definitions.
add_definitions(-DHAVE_CONFIG_H)

# Add LLM
add_library(llmservice SHARED
  llm-service.cpp
  llm-interface.cpp
)

# Add VLM
add_library(vlmservice SHARED
  vlm-service.cpp
  vlm-interface.cpp
)

# Include directories for the target
target_include_directories(llmservice PUBLIC
  ${CMAKE_SOURCE_DIR}/include
)

target_include_directories(vlmservice PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${JSONCPP_INCLUDE_DIRS}
)

# Link the library
target_link_libraries(llmservice Genie)
target_link_libraries(vlmservice Genie ${JSONCPP_LIBRARIES})

# Install the executable for LLM and VLM
install(
  TARGETS llmservice
  LIBRARY DESTINATION ${INSTALL_LIBDIR}
  PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
              GROUP_EXECUTE GROUP_READ
              WORLD_EXECUTE WORLD_READ
)

install(
  TARGETS vlmservice
  LIBRARY DESTINATION ${INSTALL_LIBDIR}
  PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
              GROUP_EXECUTE GROUP_READ
              WORLD_EXECUTE WORLD_READ
)
