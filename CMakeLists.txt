cmake_minimum_required(VERSION 3.16)
project(fastDICOMattrs VERSION 0.2.0 LANGUAGES CXX C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(FDS_BUILD_TESTS "Build fastDICOMattrs unit/integration tests" ON)
option(FDS_BUILD_BENCH "Build fastDICOMattrs benchmark scaffold" ON)
option(FDS_BUILD_ABI "Build the fastDICOMattrs C ABI shared library" ON)

add_compile_options(-Wall -Wextra)

# ---------------------------------------------------------------------------
# Core C++ library (fastdicomattrs)
# ---------------------------------------------------------------------------
add_library(fastdicomattrs
  src/tag.cpp
  src/vr.cpp
  src/transfer_syntax.cpp
  src/source.cpp
  src/value.cpp
  src/element_path.cpp
  src/element.cpp
  src/pixel_data_reference.cpp
  src/internal/container_locate.cpp
  src/internal/vr_inference.cpp
  src/dicom_structure.cpp
  src/mutation.cpp
  src/parse.cpp
  src/parser/byte_reader.cpp
  src/parser/explicit_vr_le_parser.cpp
  src/parser/implicit_vr_le_parser.cpp
  src/writer/lossless_writer.cpp
  src/dictionary.cpp
  src/dictionary_data.generated.cpp
  src/charset.cpp
  src/charset_tables.generated.cpp
)
add_library(fastdicomattrs::fastdicomattrs ALIAS fastdicomattrs)

target_include_directories(fastdicomattrs
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(fastdicomattrs PUBLIC cxx_std_20)
# Built into the fastdicomattrs_c shared library, so it must be
# position-independent even though it is itself a static library.
set_target_properties(fastdicomattrs PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# C ABI shared library (fastdicomattrs_c)
# ---------------------------------------------------------------------------
if(FDS_BUILD_ABI)
  add_library(fastdicomattrs_c SHARED
    abi/src/fds_abi.cpp
  )
  target_include_directories(fastdicomattrs_c
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/abi/include>
      $<INSTALL_INTERFACE:include>
  )
  target_link_libraries(fastdicomattrs_c PRIVATE fastdicomattrs::fastdicomattrs)
  set_target_properties(fastdicomattrs_c PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
  )
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(FDS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(FDS_BUILD_BENCH)
  add_subdirectory(bench)
endif()

# ---------------------------------------------------------------------------
# Installation and CMake package
# ---------------------------------------------------------------------------
install(TARGETS fastdicomattrs
  EXPORT fastdicomattrsTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

if(FDS_BUILD_ABI)
  install(TARGETS fastdicomattrs_c
    EXPORT fastdicomattrsTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )
  install(DIRECTORY abi/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT fastdicomattrsTargets
  FILE fastdicomattrsTargets.cmake
  NAMESPACE fastdicomattrs::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fastdicomattrs
)

configure_package_config_file(
  cmake/fastdicomattrsConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/fastdicomattrsConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fastdicomattrs
)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/fastdicomattrsConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/fastdicomattrsConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/fastdicomattrsConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fastdicomattrs
)
