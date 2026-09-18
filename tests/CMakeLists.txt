find_package(Catch2 3 REQUIRED)
include(CTest)
include(Catch)

add_executable(fds_tests
  unit/test_tag.cpp
  unit/test_vr.cpp
  unit/test_value_length.cpp
  unit/test_source.cpp
  unit/test_element_path.cpp
  unit/test_dictionary.cpp
  integration/fixture_builder.cpp
  integration/test_parse_explicit_vr_le.cpp
  integration/test_parse_implicit_vr_le.cpp
  integration/test_sequences.cpp
  integration/test_pixel_data.cpp
  integration/test_malformed.cpp
  integration/test_mutation.cpp
  integration/test_mutation_roundtrip.cpp
  integration/test_nested_insert.cpp
  integration/test_mutation_inferred.cpp
  integration/test_charset_insert.cpp
  integration/test_implicit_vr_qualification.cpp
  integration/test_private_creator.cpp
  integration/test_pixel_data_ordering.cpp
  integration/test_implicit_vr_sq_matrix.cpp
  integration/test_charset.cpp
  integration/test_charset_encode.cpp
  integration/test_roundtrip_lossless.cpp
  integration/test_write_stats.cpp
)

target_include_directories(fds_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fds_tests PRIVATE fastdicomattrs::fastdicomattrs Catch2::Catch2WithMain)

if(FDS_BUILD_ABI)
  target_sources(fds_tests PRIVATE integration/test_c_abi.cpp integration/test_c_abi_path.cpp)
  target_link_libraries(fds_tests PRIVATE fastdicomattrs_c)
endif()

catch_discover_tests(fds_tests)
