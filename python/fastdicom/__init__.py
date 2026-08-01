"""fastdicom: fast DICOM metadata extraction, backed by a C++/DCMTK core."""

from ._fastdicom import FastDicomError, get_tag, get_tags

__all__ = ["FastDicomError", "get_tag", "get_tags"]
