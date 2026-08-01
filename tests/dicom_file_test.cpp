#include "fastdicom/tags.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

const char* statusName(fastdicom::TagStatus status) {
  switch (status) {
    case fastdicom::TagStatus::found:
      return "found";
    case fastdicom::TagStatus::missing:
      return "missing";
    case fastdicom::TagStatus::error:
      return "error";
  }
  return "unknown";
}

void usage(const char* program) {
  std::cerr << "Usage: " << program << " FILE [TAG ...]\n"
            << "Example: " << program
            << " image.dcm 0010,0010 0010,0020 0008,0060\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage(argv[0]);
    return 2;
  }

  std::vector<fastdicom::Tag> tags;
  for (int index = 2; index < argc; ++index) {
    fastdicom::Tag tag;
    if (!fastdicom::parseTag(argv[index], tag)) {
      std::cerr << "Invalid tag: " << argv[index] << '\n';
      return 2;
    }
    tags.push_back(tag);
  }
  if (tags.empty()) {
    tags = {
        {0x0008, 0x0016},  // SOP Class UID
        {0x0008, 0x0060},  // Modality
        {0x0010, 0x0010},  // Patient Name
        {0x0010, 0x0020},  // Patient ID
    };
  }

  // Exercise getTags(): all requested attributes must be obtained from one load.
  const auto batch = fastdicom::getTags(argv[1], tags);
  if (batch.size() != tags.size()) {
    std::cerr << "FAIL: getTags returned an unexpected result count\n";
    return 1;
  }

  bool failed = false;
  for (std::size_t index = 0; index < tags.size(); ++index) {
    // Exercise getTag() independently and require it to agree with getTags().
    const auto single = fastdicom::getTag(argv[1], tags[index]);
    const auto& multiple = batch[index];
    const bool agrees = single.tag == multiple.tag &&
                        single.status == multiple.status &&
                        single.value == multiple.value;

    std::cout << fastdicom::formatTag(tags[index]) << '\t'
              << statusName(multiple.status);
    if (multiple.has_value()) {
      std::cout << '\t' << multiple.value;
    } else if (multiple.status == fastdicom::TagStatus::error) {
      std::cout << '\t' << multiple.message;
    }
    std::cout << '\n';

    if (!agrees) {
      std::cerr << "FAIL: getTag and getTags disagree for "
                << fastdicom::formatTag(tags[index]) << '\n';
      failed = true;
    }
    if (single.status == fastdicom::TagStatus::error ||
        multiple.status == fastdicom::TagStatus::error) {
      std::cerr << "FAIL: could not read "
                << fastdicom::formatTag(tags[index]) << '\n';
      failed = true;
    }
  }

  if (failed) {
    return 1;
  }
  std::cout << "PASS: getTag and getTags agree for " << tags.size()
            << " tag(s)\n";
  return 0;
}

