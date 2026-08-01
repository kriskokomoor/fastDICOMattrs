#include "fastdicom/tags.hpp"

#include <iostream>
#include <string_view>
#include <vector>

namespace {

void usage(const char* program) {
  std::cerr << "Usage: " << program
            << " FILE TAG [TAG ...]\nExample: " << program
            << " image.dcm 0010,0010 0008,0060\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    usage(argv[0]);
    return 2;
  }

  std::vector<fastdicom::Tag> tags;
  tags.reserve(static_cast<std::size_t>(argc - 2));
  for (int index = 2; index < argc; ++index) {
    fastdicom::Tag tag;
    if (!fastdicom::parseTag(argv[index], tag)) {
      std::cerr << "Invalid tag: " << argv[index] << '\n';
      return 2;
    }
    tags.push_back(tag);
  }

  bool had_error = false;
  for (const auto& result : fastdicom::getTags(argv[1], tags)) {
    std::cout << fastdicom::formatTag(result.tag) << '\t';
    switch (result.status) {
      case fastdicom::TagStatus::found:
        std::cout << result.value;
        break;
      case fastdicom::TagStatus::missing:
        std::cout << "<missing>";
        break;
      case fastdicom::TagStatus::error:
        std::cout << "<error: " << result.message << '>';
        had_error = true;
        break;
    }
    std::cout << '\n';
  }
  return had_error ? 1 : 0;
}
