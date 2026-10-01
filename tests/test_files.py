import io
import json
import pathlib

import pytest

import fastsimdjson
from test_lazy import to_py

DOC = {"a": [1, 2.5, "é", None, True], "b": {"c": 10**30}}


@pytest.fixture
def path(tmp_path):
    p = tmp_path / "doc.json"
    p.write_text(json.dumps(DOC), encoding="utf-8")
    return p


def test_load(path):
    with open(path, "rb") as f:
        assert fastsimdjson.load(f) == DOC
    with open(path, encoding="utf-8") as f:
        assert fastsimdjson.load(f) == DOC
    assert fastsimdjson.load(io.StringIO(json.dumps(DOC))) == DOC
    with pytest.raises(fastsimdjson.JSONDecodeError):
        fastsimdjson.load(io.BytesIO(b"{"))


def test_load_file(path):
    for p in (path, str(path), bytes(path)):
        assert fastsimdjson.load_file(p) == DOC
        assert to_py(fastsimdjson.parse_file(p)) == DOC


def test_missing_file(tmp_path):
    with pytest.raises(FileNotFoundError):
        fastsimdjson.load_file(tmp_path / "missing.json")
    with pytest.raises(FileNotFoundError):
        fastsimdjson.parse_file(tmp_path / "missing.json")


def test_invalid_file(tmp_path):
    p = tmp_path / "bad.json"
    p.write_text('{"a": ')
    with pytest.raises(fastsimdjson.JSONDecodeError):
        fastsimdjson.load_file(p)
    p.write_text("[1e400]")
    assert fastsimdjson.load_file(p) == [float("inf")]


def test_dump_load_roundtrip(tmp_path):
    p = tmp_path / "out.json"
    with open(p, "w", encoding="utf-8") as f:
        fastsimdjson.dump(DOC, f, indent=2)
    assert fastsimdjson.load_file(p) == DOC
    assert p.read_text(encoding="utf-8") == json.dumps(DOC, indent=2)
