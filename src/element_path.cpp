#include "fastdicomattrs/element_path.hpp"

#include <sstream>

namespace fds {

std::string ElementPath::to_string() const {
  std::ostringstream oss;
  for (std::size_t i = 0; i < steps_.size(); ++i) {
    if (i > 0) oss << '/';
    oss << fds::to_string(steps_[i].tag);
    if (steps_[i].item_index.has_value()) {
      oss << '[' << *steps_[i].item_index << ']';
    }
  }
  return oss.str();
}

}  // namespace fds
