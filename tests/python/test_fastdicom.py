"""Correctness tests for the fastdicom Python bindings.

Values are checked against pydicom's own reading of the same file, not
against hardcoded expectations, so this exercises the fastdicom <-> DCMTK
<-> pydicom chain rather than just fastdicom's parsing in isolation.
"""

from __future__ import annotations

import pathlib

import pydicom
import pytest
from pydicom.dataset import Dataset, FileMetaDataset
from pydicom.multival import MultiValue
from pydicom.uid import ExplicitVRLittleEndian, SecondaryCaptureImageStorage, generate_uid

import fastdicom

TAGS = {
    "PatientName": "0010,0010",
    "PatientID": "0010,0020",
    "Modality": "0008,0060",
    "SOPClassUID": "0008,0016",
    "SOPInstanceUID": "0008,0018",
    "StudyInstanceUID": "0020,000d",
    "SeriesInstanceUID": "0020,000e",
}

# Absent from sample_dataset, used to test the missing-attribute path.
ABSENT_TAG = "0010,1010"  # Patient's Age


@pytest.fixture
def sample_dataset() -> Dataset:
    meta = FileMetaDataset()
    meta.MediaStorageSOPClassUID = SecondaryCaptureImageStorage
    meta.MediaStorageSOPInstanceUID = generate_uid()
    meta.TransferSyntaxUID = ExplicitVRLittleEndian

    dataset = Dataset()
    dataset.file_meta = meta
    dataset.SOPClassUID = SecondaryCaptureImageStorage
    dataset.SOPInstanceUID = meta.MediaStorageSOPInstanceUID
    dataset.PatientName = "Test^Patient"
    dataset.PatientID = "ID0001"
    dataset.Modality = "CT"
    dataset.StudyInstanceUID = generate_uid()
    dataset.SeriesInstanceUID = generate_uid()
    return dataset


@pytest.fixture
def sample_file(tmp_path: pathlib.Path, sample_dataset: Dataset) -> pathlib.Path:
    path = tmp_path / "sample.dcm"
    sample_dataset.save_as(path, enforce_file_format=True)
    return path


def pydicom_value(dataset: Dataset, keyword: str) -> str | None:
    """DCMTK represents multi-valued elements backslash-joined as one
    string; reproduce that here so fastdicom's return value is directly
    comparable to pydicom's."""
    if keyword not in dataset:
        return None
    value = dataset[keyword].value
    if isinstance(value, MultiValue):
        return "\\".join(str(v) for v in value)
    return str(value)


@pytest.mark.parametrize("keyword", list(TAGS))
def test_get_tag_matches_pydicom(sample_file, sample_dataset, keyword):
    expected = pydicom_value(sample_dataset, keyword)
    actual = fastdicom.get_tag(str(sample_file), TAGS[keyword])
    assert actual == expected


def test_get_tag_missing_attribute_returns_none(sample_file):
    assert fastdicom.get_tag(str(sample_file), ABSENT_TAG) is None


def test_get_tags_matches_pydicom(sample_file, sample_dataset):
    result = fastdicom.get_tags(str(sample_file), list(TAGS.values()))
    for keyword, tag in TAGS.items():
        assert result[tag] == pydicom_value(sample_dataset, keyword)


def test_get_tags_includes_missing_as_none(sample_file):
    result = fastdicom.get_tags(str(sample_file), [TAGS["PatientName"], ABSENT_TAG])
    assert result[TAGS["PatientName"]] is not None
    assert result[ABSENT_TAG] is None


def test_get_tags_duplicate_tag_collapses_to_one_key(sample_file):
    tag = TAGS["PatientName"]
    result = fastdicom.get_tags(str(sample_file), [tag, tag])
    assert list(result.keys()) == [tag]


def test_get_tags_empty_list_returns_empty_dict(sample_file):
    assert fastdicom.get_tags(str(sample_file), []) == {}


def test_get_tag_invalid_tag_raises_value_error(sample_file):
    with pytest.raises(ValueError):
        fastdicom.get_tag(str(sample_file), "not-a-tag")


def test_get_tag_missing_file_raises_fastdicom_error(tmp_path):
    missing = tmp_path / "does-not-exist.dcm"
    with pytest.raises(fastdicom.FastDicomError):
        fastdicom.get_tag(str(missing), TAGS["PatientName"])


def test_pydicom_test_data_agrees_with_fastdicom():
    """Cross-check against a real-world DICOM file, not just synthetic ones."""
    path = pydicom.data.get_testdata_file("CT_small.dcm")
    dataset = pydicom.dcmread(path, stop_before_pixels=True)

    result = fastdicom.get_tags(str(path), list(TAGS.values()))
    for keyword, tag in TAGS.items():
        assert result[tag] == pydicom_value(dataset, keyword)
