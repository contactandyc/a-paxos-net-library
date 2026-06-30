# SPDX-FileCopyrightText: 2026 Andy Curtis <contactandyc@gmail.com>
# SPDX-License-Identifier: Apache-2.0
#
# Maintainer: Andy Curtis <contactandyc@gmail.com>

cmake_minimum_required(VERSION 3.20)

# A dynamically generated examples project.
project(a_paxos_net_library_basic LANGUAGES C)

if(CMAKE_PREFIX_PATH)
  include_directories("${CMAKE_PREFIX_PATH}/include")
  link_directories("${CMAKE_PREFIX_PATH}/lib")
endif()

find_library(M_LIB m)

find_package(h2o_c_library CONFIG REQUIRED)
find_package(the_macro_library CONFIG REQUIRED)
find_package(a_paxos_core CONFIG REQUIRED)

# ---- Targets ----
add_executable(basic_paxos
  src/main.c
)

target_include_directories(basic_paxos PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set_target_properties(basic_paxos PROPERTIES
  C_STANDARD 23
  C_STANDARD_REQUIRED YES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED YES
)

# Ensure examples running in library stacks link the main library!
if(NOT TARGET a_paxos_net_library::a_paxos_net_library)
  find_package(a_paxos_net_library CONFIG REQUIRED)
endif()
# target_link_libraries(basic_paxos PRIVATE a_paxos_net_library::a_paxos_net_library)

target_link_libraries(basic_paxos PRIVATE
  h2o_c_library::h2o_c_library
  the_macro_library::the_macro_library
  a_paxos_core::a_paxos_core
  a_paxos_net_library::a_paxos_net_library
)

if(M_LIB)
  target_link_libraries(basic_paxos PRIVATE ${M_LIB})
endif()

if(MSVC)
  target_compile_options(basic_paxos PRIVATE /W4)
else()
  target_compile_options(basic_paxos PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS basic_paxos RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
