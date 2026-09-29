# Rack Turnup Manager - build, test and packaging entry point.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

cmake_minimum_required(VERSION 3.20)

project(rack_turnup
  VERSION 1.0.0
  DESCRIPTION "Rack-level commissioning coordination for the Data Center Control Plane"
  LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(RACK_TURNUP_IS_TOP_LEVEL ON)
else()
  set(RACK_TURNUP_IS_TOP_LEVEL OFF)
endif()

option(RACK_TURNUP_BUILD_SHARED "Build Rack Turnup Manager as a shared library" OFF)
option(RACK_TURNUP_BUILD_CLI "Build the rack-turnup command line tool" ON)
option(RACK_TURNUP_BUILD_TESTS "Build the proof-obligation test suite" ON)
option(RACK_TURNUP_BUILD_BENCHMARKS "Build the benchmark tool" ON)
option(RACK_TURNUP_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(RACK_TURNUP_ENABLE_ASAN "Build with AddressSanitizer where the toolchain supports it" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Strict first-party warnings. The library is dependency-light by design, so
# every warning that appears here belongs to this repository.
set(RACK_TURNUP_WARNING_FLAGS)
if(MSVC)
  list(APPEND RACK_TURNUP_WARNING_FLAGS /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
  if(RACK_TURNUP_WARNINGS_AS_ERRORS)
    list(APPEND RACK_TURNUP_WARNING_FLAGS /WX)
  endif()
else()
  list(APPEND RACK_TURNUP_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion
       -Wsign-conversion -Wold-style-cast -Wcast-qual -Wundef -Wnon-virtual-dtor)
  if(RACK_TURNUP_WARNINGS_AS_ERRORS)
    list(APPEND RACK_TURNUP_WARNING_FLAGS -Werror)
  endif()
endif()

function(rack_turnup_apply_warnings target)
  target_compile_options(${target} PRIVATE ${RACK_TURNUP_WARNING_FLAGS})
  if(RACK_TURNUP_ENABLE_ASAN)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
  endif()
endfunction()

if(MSVC)
  set(RACK_TURNUP_BUILD_COMPILER "MSVC ${CMAKE_CXX_COMPILER_VERSION}")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(RACK_TURNUP_BUILD_COMPILER "GCC ${CMAKE_CXX_COMPILER_VERSION}")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(RACK_TURNUP_BUILD_COMPILER "Clang ${CMAKE_CXX_COMPILER_VERSION}")
else()
  set(RACK_TURNUP_BUILD_COMPILER "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif()

set(RACK_TURNUP_LIBRARY_TYPE STATIC)
if(RACK_TURNUP_BUILD_SHARED)
  set(RACK_TURNUP_LIBRARY_TYPE SHARED)
endif()

set(RACK_TURNUP_SOURCES
  src/authorization.cpp
  src/cli.cpp
  src/composition.cpp
  src/digest.cpp
  src/errors.cpp
  src/evaluation.cpp
  src/evidence.cpp
  src/ids.cpp
  src/model.cpp
  src/observation.cpp
  src/plan.cpp
  src/service.cpp
  src/state.cpp
  src/state_codec.cpp
  src/store.cpp
  src/text.cpp
  src/time.cpp
  src/version.cpp
)

add_library(rack_turnup ${RACK_TURNUP_LIBRARY_TYPE} ${RACK_TURNUP_SOURCES})
add_library(rack_turnup::rack_turnup ALIAS rack_turnup)

target_compile_features(rack_turnup PUBLIC cxx_std_20)
set_target_properties(rack_turnup PROPERTIES
  CXX_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON
  DEFINE_SYMBOL RACK_TURNUP_BUILDING)

if(RACK_TURNUP_BUILD_SHARED)
  set_target_properties(rack_turnup PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
  target_compile_definitions(rack_turnup PUBLIC RACK_TURNUP_SHARED)
endif()

target_compile_definitions(rack_turnup PRIVATE
  RACK_TURNUP_VERSION_STRING="${PROJECT_VERSION}"
  RACK_TURNUP_BUILD_CONFIGURATION="$<CONFIG>"
  RACK_TURNUP_BUILD_COMPILER="${RACK_TURNUP_BUILD_COMPILER}")

target_include_directories(rack_turnup PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

rack_turnup_apply_warnings(rack_turnup)

# The command line tool is a thin main() over the library, so the same code can
# be embedded by a consumer through cli.hpp.
if(RACK_TURNUP_BUILD_CLI)
  add_executable(rack_turnup_cli app/main.cpp)
  target_link_libraries(rack_turnup_cli PRIVATE rack_turnup::rack_turnup)
  rack_turnup_apply_warnings(rack_turnup_cli)
  set_target_properties(rack_turnup_cli PROPERTIES OUTPUT_NAME rack-turnup)
  install(TARGETS rack_turnup_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---------------------------------------------------------------------------
# Installation, export and packaging.
# ---------------------------------------------------------------------------

install(TARGETS rack_turnup
  EXPORT rack_turnupTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT rack_turnupTargets
  FILE rack_turnupTargets.cmake
  NAMESPACE rack_turnup::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rack_turnup)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/rack_turnupConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/rack_turnupConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rack_turnup)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/rack_turnupConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/rack_turnupConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/rack_turnupConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rack_turnup)

install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/rack_turnup)

# ---------------------------------------------------------------------------
# Proof obligations.
# ---------------------------------------------------------------------------

if(RACK_TURNUP_BUILD_TESTS AND RACK_TURNUP_IS_TOP_LEVEL)
  enable_testing()

  # The child process used to prove crash consistency and cross-process
  # exclusion on real operating-system processes.
  add_executable(rtm_crash_child tests/crash_child.cpp)
  target_link_libraries(rtm_crash_child PRIVATE rack_turnup::rack_turnup)
  rack_turnup_apply_warnings(rtm_crash_child)

  add_executable(rtm_tests
    tests/harness.cpp
    tests/test_core.cpp
    tests/test_evaluation.cpp
    tests/test_service.cpp
    tests/test_store.cpp)
  target_link_libraries(rtm_tests PRIVATE rack_turnup::rack_turnup)
  rack_turnup_apply_warnings(rtm_tests)
  target_compile_definitions(rtm_tests PRIVATE
    RTM_CRASH_CHILD_PATH="$<TARGET_FILE:rtm_crash_child>"
    RTM_CLI_PATH="$<TARGET_FILE:rack_turnup_cli>")
  add_dependencies(rtm_tests rtm_crash_child rack_turnup_cli)

  add_test(NAME rtm_tests COMMAND rtm_tests)

  # Installed-artifact validation: install into a private prefix, configure an
  # independent consumer with find_package(), build it, run it, and check that
  # it reports a verdict from the installed library.
  add_test(NAME installed_artifact
    COMMAND ${CMAKE_COMMAND}
      -DBUILD_DIR=${CMAKE_BINARY_DIR}
      -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      -DWORK_DIR=${CMAKE_BINARY_DIR}/installed-artifact
      -DGENERATOR=${CMAKE_GENERATOR}
      -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/packaging/installed_artifact.cmake)
  add_dependencies(rtm_tests rack_turnup)
endif()

if(RACK_TURNUP_BUILD_BENCHMARKS AND RACK_TURNUP_IS_TOP_LEVEL)
  add_executable(rtm_bench bench/bench.cpp)
  target_link_libraries(rtm_bench PRIVATE rack_turnup::rack_turnup)
  rack_turnup_apply_warnings(rtm_bench)
endif()
