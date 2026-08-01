#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "fastdicom/tags.hpp"

namespace py = pybind11;

namespace {

class FastDicomError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

fastdicom::Tag parseTagOrThrow(const std::string& text) {
  fastdicom::Tag tag;
  if (!fastdicom::parseTag(text, tag)) {
    throw py::value_error("invalid DICOM tag: " + text);
  }
  return tag;
}

// Missing attributes are a normal outcome (None); read/parse failures raise.
py::object toPython(const fastdicom::TagResult& result) {
  switch (result.status) {
    case fastdicom::TagStatus::found:
      return py::cast(result.value);
    case fastdicom::TagStatus::missing:
      return py::none();
    case fastdicom::TagStatus::error:
      throw FastDicomError(result.message);
  }
  throw FastDicomError("unknown tag status");
}

py::object getTag(const std::string& filename, const std::string& tag) {
  return toPython(fastdicom::getTag(filename, parseTagOrThrow(tag)));
}

py::dict getTags(const std::string& filename,
                  const std::vector<std::string>& tags) {
  std::vector<fastdicom::Tag> parsed;
  parsed.reserve(tags.size());
  for (const auto& tag : tags) {
    parsed.push_back(parseTagOrThrow(tag));
  }
  const auto results = fastdicom::getTags(filename, parsed);

  py::dict output;
  for (std::size_t index = 0; index < tags.size(); ++index) {
    output[py::str(tags[index])] = toPython(results[index]);
  }
  return output;
}

}  // namespace

PYBIND11_MODULE(_fastdicom, module) {
  module.doc() = "Fast DICOM metadata extraction, backed by fastdicom/DCMTK";

  py::register_exception<FastDicomError>(module, "FastDicomError",
                                          PyExc_RuntimeError);

  module.def("get_tag", &getTag, py::arg("filename"), py::arg("tag"),
             "Return the string value of one DICOM tag, or None if the "
             "attribute is absent from the file.");
  module.def("get_tags", &getTags, py::arg("filename"), py::arg("tags"),
             "Return a dict mapping each requested tag string to its value "
             "(or None if absent). The file is parsed once for all tags.");
}
