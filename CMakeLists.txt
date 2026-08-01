cmake_minimum_required(VERSION 3.16)

project(fastDICOM VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(FASTDICOM_BUILD_CLI "Build the command-line example" ON)
option(FASTDICOM_BUILD_TESTS "Build tests" ON)
option(FASTDICOM_BUILD_PYTHON "Build the pybind11 Python bindings" OFF)

find_package(DCMTK REQUIRED)

add_library(fastdicom src/tags.cpp)
add_library(fastDICOM::fastdicom ALIAS fastdicom)
target_compile_features(fastdicom PUBLIC cxx_std_20)
target_compile_options(fastdicom PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>)
target_include_directories(fastdicom
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE ${DCMTK_INCLUDE_DIRS})
if(TARGET DCMTK::dcmdata)
  target_link_libraries(fastdicom PUBLIC DCMTK::dcmdata DCMTK::ofstd)
elseif(TARGET dcmdata)
  target_link_libraries(fastdicom PUBLIC dcmdata ofstd)
else()
  target_link_libraries(fastdicom PUBLIC ${DCMTK_LIBRARIES})
endif()

include(GNUInstallDirs)
install(TARGETS fastdicom EXPORT fastDICOMTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT fastDICOMTargets
  FILE fastDICOMTargets.cmake
  NAMESPACE fastDICOM::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fastDICOM)
include(CMakePackageConfigHelpers)
configure_package_config_file(
  cmake/fastDICOMConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/fastDICOMConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fastDICOM)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/fastDICOMConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/fastDICOMConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/fastDICOMConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/fastDICOM)

if(FASTDICOM_BUILD_CLI)
  add_executable(fastdicom-cli examples/fastdicom.cpp)
  target_link_libraries(fastdicom-cli PRIVATE fastdicom)
endif()
include(CTest)
if(BUILD_TESTING AND FASTDICOM_BUILD_TESTS)
  add_executable(fastdicom-tests tests/tags_test.cpp)
  target_include_directories(fastdicom-tests PRIVATE ${DCMTK_INCLUDE_DIRS})
  target_link_libraries(fastdicom-tests PRIVATE fastdicom)
  add_test(NAME fastdicom-tests COMMAND fastdicom-tests)

  add_executable(fastdicom-file-test tests/dicom_file_test.cpp)
  target_link_libraries(fastdicom-file-test PRIVATE fastdicom)
endif()

if(FASTDICOM_BUILD_PYTHON)
  find_package(pybind11 CONFIG REQUIRED)
  pybind11_add_module(_fastdicom python/fastdicom/bindings.cpp)
  target_link_libraries(_fastdicom PRIVATE fastdicom)
  install(TARGETS _fastdicom LIBRARY DESTINATION fastdicom)
  install(FILES python/fastdicom/__init__.py DESTINATION fastdicom)
endif()
