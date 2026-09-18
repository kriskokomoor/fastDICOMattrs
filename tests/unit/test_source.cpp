#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "fastdicomattrs/source.hpp"

using fds::FileSource;
using fds::MemorySource;
using fds::SourceSpan;

TEST_CASE("MemorySource::own resolves spans into its owned buffer", "[source]") {
  std::vector<std::byte> data = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  auto source = MemorySource::own(data, "test");
  REQUIRE(source->size() == 4);

  const std::byte* ptr = nullptr;
  REQUIRE(source->try_get(SourceSpan(1, 2), &ptr));
  REQUIRE(ptr[0] == std::byte{2});
  REQUIRE(ptr[1] == std::byte{3});
}

TEST_CASE("MemorySource::view resolves spans into caller memory without copying", "[source]") {
  std::vector<std::byte> data = {std::byte{9}, std::byte{8}, std::byte{7}};
  auto source = MemorySource::view(data.data(), data.size(), "view");
  const std::byte* ptr = nullptr;
  REQUIRE(source->try_get(SourceSpan(0, 3), &ptr));
  REQUIRE(ptr == data.data());  // genuinely zero-copy: same address as caller's buffer
}

TEST_CASE("Source::try_get rejects out-of-range spans", "[source]") {
  std::vector<std::byte> data = {std::byte{1}, std::byte{2}};
  auto source = MemorySource::own(data, "test");
  const std::byte* ptr = nullptr;
  REQUIRE_FALSE(source->try_get(SourceSpan(1, 5), &ptr));
  REQUIRE_FALSE(source->try_get(SourceSpan(10, 1), &ptr));
}

TEST_CASE("FileSource reads back exactly what was written", "[source]") {
  auto path = std::filesystem::temp_directory_path() / "fds_test_source_file.bin";
  {
    std::ofstream f(path, std::ios::binary);
    f.write("HELLOWORLD", 10);
  }
  std::string error;
  auto source = FileSource::open(path, &error);
  REQUIRE(source != nullptr);
  REQUIRE(source->size() == 10);
  const std::byte* ptr = nullptr;
  REQUIRE(source->try_get(SourceSpan(5, 5), &ptr));
  REQUIRE(std::string(reinterpret_cast<const char*>(ptr), 5) == "WORLD");

  std::filesystem::remove(path);
}

TEST_CASE("FileSource::open reports an error for a missing file", "[source]") {
  std::string error;
  auto source = FileSource::open("/nonexistent/path/does/not/exist.dcm", &error);
  REQUIRE(source == nullptr);
  REQUIRE_FALSE(error.empty());
}
