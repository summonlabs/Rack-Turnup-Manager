# Installed-artifact validation.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Run with: cmake -DBUILD_DIR=... -DSOURCE_DIR=... -DWORK_DIR=... -DGENERATOR=...
# -DCMAKE_COMMAND=... -P installed_artifact.cmake
#
# The script installs the built library into a private prefix, configures an
# independent consumer project with find_package() against that prefix, builds
# it, runs it, and checks the output. Nothing in the consumer build refers to
# the library source tree.

if(NOT DEFINED BUILD_DIR OR NOT DEFINED SOURCE_DIR OR NOT DEFINED WORK_DIR)
  message(FATAL_ERROR "BUILD_DIR, SOURCE_DIR and WORK_DIR are required")
endif()

set(prefix "${WORK_DIR}/prefix")
set(consumer_build "${WORK_DIR}/consumer-build")

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

message(STATUS "installing into ${prefix}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${prefix}"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "install failed: ${install_output} ${install_error}")
endif()

# The package must ship a config, a version file, an export file and headers.
foreach(required
    "${prefix}/lib/cmake/rack_turnup/rack_turnupConfig.cmake"
    "${prefix}/lib/cmake/rack_turnup/rack_turnupConfigVersion.cmake"
    "${prefix}/lib/cmake/rack_turnup/rack_turnupTargets.cmake"
    "${prefix}/include/rack_turnup/service.hpp"
    "${prefix}/include/rack_turnup/service.hpp")
  if(NOT EXISTS "${required}")
    message(FATAL_ERROR "the installed package is incomplete: ${required} is missing")
  endif()
endforeach()

file(READ "${prefix}/lib/cmake/rack_turnup/rack_turnupTargets.cmake" targets_text)
if(NOT targets_text MATCHES "rack_turnup::rack_turnup")
  message(FATAL_ERROR "the exported target rack_turnup::rack_turnup is missing")
endif()

message(STATUS "configuring the out-of-tree consumer")
set(configure_command "${CMAKE_COMMAND}"
    -S "${SOURCE_DIR}/tests/packaging/consumer"
    -B "${consumer_build}"
    "-DCMAKE_PREFIX_PATH=${prefix}"
    "-DCMAKE_BUILD_TYPE=Release")
if(DEFINED GENERATOR AND NOT GENERATOR STREQUAL "")
  list(APPEND configure_command -G "${GENERATOR}")
endif()
if(DEFINED CXX_COMPILER AND NOT CXX_COMPILER STREQUAL "")
  list(APPEND configure_command "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
endif()
execute_process(
  COMMAND ${configure_command}
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR "consumer configure failed: ${configure_output} ${configure_error}")
endif()

message(STATUS "building the out-of-tree consumer")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}" --config Release
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR "consumer build failed: ${build_output} ${build_error}")
endif()

message(STATUS "running the out-of-tree consumer")
set(consumer_state "${WORK_DIR}/consumer-state.rtm")
execute_process(
  COMMAND "${consumer_build}/rack_turnup_consumer" "${consumer_state}"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_output
  ERROR_VARIABLE run_error)
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR "consumer run failed: ${run_output} ${run_error}")
endif()
if(NOT run_output MATCHES "verdict=")
  message(FATAL_ERROR "the consumer did not report a verdict: ${run_output}")
endif()
if(NOT run_output MATCHES "verdict_digest=")
  message(FATAL_ERROR "the consumer did not report a verdict digest: ${run_output}")
endif()
if(NOT EXISTS "${consumer_state}.watermark")
  message(FATAL_ERROR "the consumer did not publish a watermarked generation")
endif()

message(STATUS "installed artifact validation succeeded")
message(STATUS "${run_output}")
