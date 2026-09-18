#include "fastdicomattrs/tag.hpp"

#include <cstdio>

namespace fds {

std::string to_string(Tag tag) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "(%04X,%04X)", tag.group, tag.element);
  return std::string(buf);
}

}  // namespace fds
