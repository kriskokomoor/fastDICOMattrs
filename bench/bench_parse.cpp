// Minimal benchmark scaffold: no external benchmark library dependency
// (keeps implementation dependencies minimal, per the charter). Reports
// wall-clock timings for parsing and traversal on a synthetically
// generated dataset. Numbers here are a starting point for future FAST-mode
// optimization decisions, not a claim about real-world performance -- see
// docs/architecture.md section 6 ("FAST"). This is still a synthetic-data
// scaffold, unlike bench/bench_compare.py, which is the completed,
// real-corpus, pydicom-comparative benchmark behind docs/benchmarks.md --
// the two serve different purposes and neither supersedes the other.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "fastdicomattrs/fastdicomattrs.hpp"

namespace {

std::vector<std::byte> ascii(const std::string& s) {
  std::vector<std::byte> out(s.size());
  std::memcpy(out.data(), s.data(), s.size());
  return out;
}

void put_u16(std::vector<std::byte>& out, std::uint16_t v) {
  out.push_back(static_cast<std::byte>(v & 0xFF));
  out.push_back(static_cast<std::byte>((v >> 8) & 0xFF));
}
void put_tag(std::vector<std::byte>& out, std::uint16_t g, std::uint16_t e) {
  put_u16(out, g);
  put_u16(out, e);
}
void put_element_short(std::vector<std::byte>& out, std::uint16_t g, std::uint16_t e,
                        const char* vr, const std::string& value) {
  put_tag(out, g, e);
  out.push_back(static_cast<std::byte>(vr[0]));
  out.push_back(static_cast<std::byte>(vr[1]));
  put_u16(out, static_cast<std::uint16_t>(value.size()));
  auto v = ascii(value);
  out.insert(out.end(), v.begin(), v.end());
}

std::vector<std::byte> build_synthetic_dataset(std::size_t element_count) {
  std::vector<std::byte> group_body;
  put_element_short(group_body, 0x0002, 0x0010, "UI", "1.2.840.10008.1.2.1");

  std::vector<std::byte> out;
  out.insert(out.end(), 128, std::byte{0});
  auto dicm = ascii("DICM");
  out.insert(out.end(), dicm.begin(), dicm.end());
  put_element_short(out, 0x0002, 0x0000, "UL", std::string());  // placeholder, value fixed below
  // (kept minimal: parser does not require a correct group length; see
  // src/parser/explicit_vr_le_parser.cpp)
  out.insert(out.end(), group_body.begin(), group_body.end());

  for (std::size_t i = 0; i < element_count; ++i) {
    std::uint16_t element = static_cast<std::uint16_t>(0x1000 + (i % 4000));
    put_element_short(out, 0x0009, element, "SH", "VALUE0123");
  }
  return out;
}

template <typename Fn>
double time_ms(Fn&& fn) {
  auto start = std::chrono::steady_clock::now();
  fn();
  auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::milli>(end - start).count();
}

}  // namespace

int main() {
  constexpr std::size_t kElementCount = 50'000;
  auto data = build_synthetic_dataset(kElementCount);
  std::printf("Synthetic dataset: %zu elements, %zu bytes\n", kElementCount, data.size());

  fds::ParseResult result;
  double parse_ms = time_ms([&] { result = fds::parse_buffer(data); });
  std::printf("parse_buffer (Fidelity::Standard): %.3f ms (%.1f elements/ms)\n", parse_ms,
              kElementCount / parse_ms);

  if (result.structure == nullptr) {
    std::fprintf(stderr, "benchmark dataset failed to parse; see diagnostics\n");
    for (const auto& d : result.diagnostics) {
      std::fprintf(stderr, "  [%d] %s\n", static_cast<int>(d.severity), d.message.c_str());
    }
    return 1;
  }

  std::size_t visited = 0;
  double visit_ms = time_ms([&] {
    result.structure->visit([&](const fds::Element&, const fds::ElementPath&) { ++visited; });
  });
  std::printf("visit() traversal: %.3f ms for %zu elements (%.1f elements/ms)\n", visit_ms, visited,
              visited / visit_ms);

  // find(Tag) is a linear scan over top-level elements (see
  // docs/architecture.md: DICOMStructure is order-preserving, not a map).
  // Only a small, fixed number of lookups are timed here -- timing
  // kElementCount lookups against kElementCount elements would be an
  // intentionally-quadratic measurement, not a useful throughput number.
  constexpr std::size_t kLookupCount = 2'000;
  std::size_t found = 0;
  double find_ms = time_ms([&] {
    for (std::size_t i = 0; i < kLookupCount; ++i) {
      std::uint16_t element = static_cast<std::uint16_t>(0x1000 + (i % 4000));
      if (result.structure->find(fds::Tag(0x0009, element)) != nullptr) ++found;
    }
  });
  std::printf("find(Tag) x%zu (linear scan over %zu elements): %.3f ms (%.1f lookups/ms)\n",
              kLookupCount, kElementCount, find_ms, kLookupCount / find_ms);

  return 0;
}
