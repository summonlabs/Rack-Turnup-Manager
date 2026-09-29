# An independent out-of-tree consumer of the installed Rack Turnup Manager
# package. It is configured with find_package() against an installed prefix and
# never sees the source tree of the library itself.

cmake_minimum_required(VERSION 3.20)

project(rack_turnup_consumer LANGUAGES CXX)

find_package(rack_turnup 1.0 REQUIRED)

add_executable(rack_turnup_consumer main.cpp)
target_link_libraries(rack_turnup_consumer PRIVATE rack_turnup::rack_turnup)
