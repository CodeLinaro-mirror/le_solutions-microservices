cmake_minimum_required(VERSION 3.8.2)
project(LLM_SERVICE LANGUAGES CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

# Include directories
include_directories(${SYSROOT_INCDIR})

# Link directories
link_directories(${SYSROOT_LIBDIR})

# Common compiler flags.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unused-parameter")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Werror")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-parameter")

# Precompiler definitions.
add_definitions(-DHAVE_CONFIG_H)

# Add executable
add_library(llmservice SHARED
  llm-service.cpp
  llm-interface.cpp
)

# Include directories for the target
target_include_directories(llmservice PUBLIC
  ${CMAKE_SOURCE_DIR}/include
)

# Link the library
target_link_libraries(llmservice Genie)

# Install the executable
install(
  TARGETS llmservice
  LIBRARY DESTINATION ${INSTALL_LIBDIR}
  PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
              GROUP_EXECUTE GROUP_READ
              WORLD_EXECUTE WORLD_READ
)
