#include "fastdicomattrs/vr.hpp"

#include <array>
#include <utility>

namespace fds {
namespace {

constexpr std::pair<std::string_view, VR> kTable[] = {
    {"AE", VR::AE}, {"AS", VR::AS}, {"AT", VR::AT}, {"CS", VR::CS}, {"DA", VR::DA},
    {"DS", VR::DS}, {"DT", VR::DT}, {"FL", VR::FL}, {"FD", VR::FD}, {"IS", VR::IS},
    {"LO", VR::LO}, {"LT", VR::LT}, {"OB", VR::OB}, {"OD", VR::OD}, {"OF", VR::OF},
    {"OL", VR::OL}, {"OV", VR::OV}, {"OW", VR::OW}, {"PN", VR::PN}, {"SH", VR::SH},
    {"SL", VR::SL}, {"SQ", VR::SQ}, {"SS", VR::SS}, {"ST", VR::ST}, {"SV", VR::SV},
    {"TM", VR::TM}, {"UC", VR::UC}, {"UI", VR::UI}, {"UL", VR::UL}, {"UN", VR::UN},
    {"UR", VR::UR}, {"US", VR::US}, {"UT", VR::UT}, {"UV", VR::UV},
};

}  // namespace

std::optional<VR> vr_from_string(std::string_view text) noexcept {
  for (const auto& [name, vr] : kTable) {
    if (name == text) return vr;
  }
  return std::nullopt;
}

std::string_view to_string(VR vr) noexcept {
  for (const auto& [name, candidate] : kTable) {
    if (candidate == vr) return name;
  }
  return "UN";  // VR::Unknown, and any other unmapped value: safe fallback.
}

}  // namespace fds
