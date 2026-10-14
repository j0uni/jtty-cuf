cmake_minimum_required(VERSION 3.20)

project(jtty VERSION 0.1.0 DESCRIPTION "JTTY text-over-audio codec, transmitter and receiver" LANGUAGES CXX)

option(JTTY_BUILD_EXAMPLES "Build the jtty_encode and jtty_decode example tools" ON)
option(JTTY_BUILD_TESTS "Build the tests" ON)
option(JTTY_WERROR "Treat compiler warnings as errors" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)

add_library(jtty
  src/protocol.cpp
  src/message_codec.cpp
  src/receiver.cpp
  src/receiver_assembly.cpp
  src/receiver_dsp.cpp
  src/receiver_fec.cpp)
add_library(jtty::jtty ALIAS jtty)

target_include_directories(jtty
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(jtty PUBLIC cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(jtty PRIVATE -Wall -Wextra -Wpedantic)
  if(JTTY_WERROR)
    target_compile_options(jtty PRIVATE -Werror)
  endif()
  # The receiver search is arithmetic-bound; optimize it even in unoptimized
  # developer builds so a reception finishes within its documented time.
  set_source_files_properties(
    src/receiver.cpp src/receiver_dsp.cpp src/receiver_fec.cpp src/receiver_assembly.cpp
    PROPERTIES COMPILE_OPTIONS "-O2")
elseif(MSVC)
  target_compile_options(jtty PRIVATE /W4)
  if(JTTY_WERROR)
    target_compile_options(jtty PRIVATE /WX)
  endif()
endif()

if(JTTY_BUILD_EXAMPLES)
  add_executable(jtty_encode examples/jtty_encode.cpp)
  target_link_libraries(jtty_encode PRIVATE jtty::jtty)
  add_executable(jtty_decode examples/jtty_decode.cpp)
  target_link_libraries(jtty_decode PRIVATE jtty::jtty)
endif()

if(JTTY_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Installation and the CMake package: find_package(jtty) then link jtty::jtty.
install(TARGETS jtty
  EXPORT jttyTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/jtty DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(JTTY_BUILD_EXAMPLES)
  install(TARGETS jtty_encode jtty_decode RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

include(CMakePackageConfigHelpers)
install(EXPORT jttyTargets
  FILE jttyTargets.cmake
  NAMESPACE jtty::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/jtty)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/jttyConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/jttyConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/jttyConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/jtty)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/jttyConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/jttyConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/jtty)
