import glob
import json
import os

import pytest

import fastsimdjson

DATA = os.environ.get("JSONDIR", "simdjson-data/jsonexamples")
FILES = sorted(glob.glob(os.path.join(DATA, "*.json")))

# ASCII whitespace that JSON treats as insignificant.
WS = b" \t\n\r"


def reference_minify(data: bytes) -> bytes:
    """The simdjson minifier: drop ASCII whitespace outside strings.

    An unterminated string is an error. Nothing else is validated.
    """
    out = bytearray()
    in_string = False
    escape = False
    for c in data:
        if in_string:
            out.append(c)
            if escape:
                escape = False
            elif c == 0x5C:  # backslash
                escape = True
            elif c == 0x22:  # quote
                in_string = False
        elif c in WS:
            continue
        else:
            out.append(c)
            if c == 0x22:
                in_string = True
                escape = False
    if in_string:
        raise ValueError("unclosed string")
    return bytes(out)


def check(data: bytes):
    got = fastsimdjson.minify(data)
    assert got == reference_minify(data)
    assert isinstance(got, bytes)


@pytest.mark.parametrize(
    "doc",
    [
        b"",
        b" ",
        b" \t\r\n ",
        b"0",
        b"1.00",
        b"1e2",
        b"01",
        b"true",
        b"false",
        b"null",
        b"[]",
        b"{}",
        b'""',
        b'" "',
        b'"a b"',
        b"[1, 2]",
        b'{ "a" : [1, 2] }',
        b'{ "a" : " x\ty " }',
        b'{ "a" : "say \\"hi\\"" }',
        b'{"a":1,"a":2}',
        b'{ "a" : 1, "a" : 2 }',
        b"[1, 2,]",
        b"not json",
        b" \n not \t json \r ",
        b"[\n\t1,\r\n2\n]",
        " \n{ \"é\" : \"a b\" }\t".encode(),
        b'{ "a" : "\xc2\xa0" }',
        b"\xc2\xa0[ 1 ]",
        b'"foo\\\\"',
        b'"\\u00e9"',
    ],
)
def test_cases(doc):
    check(doc)


@pytest.mark.parametrize("n", [0, 1, 2, 31, 32, 63, 64, 65, 127, 128, 129, 200, 255, 256, 1000, 4096])
def test_block_edges(n):
    pad = b" " * n
    check(pad)
    check(pad + b"\t" * n + b"\n" * (n % 7) + b"\r" * (n % 5))
    check(pad + b"[1, 2]" + pad)
    check(pad + b'{"a":' + pad + b'"' + b"x " * (n % 50) + b'"}' + pad)
    # A string longer than a SIMD block, with spaces that must be kept.
    check(pad + b'"' + b"ab " * n + b'"' + pad)


def test_pretty_round_trip():
    obj = {"a": [1, 2.5, "x y", True, None], "b": {"c": "d"}}
    pretty = json.dumps(obj, indent=2).encode()
    got = fastsimdjson.minify(pretty)
    assert got == json.dumps(obj, separators=(",", ":")).encode()
    assert json.loads(got) == obj
    # Number text is kept, not reparsed.
    assert fastsimdjson.minify(b" 1.00 ") == b"1.00"
    assert fastsimdjson.minify(b"[1e2, 1.50]") == b"[1e2,1.50]"


def test_str_utf8():
    assert fastsimdjson.minify('{ "é" : "a b" }') == '{"é":"a b"}'.encode()
    with pytest.raises(UnicodeEncodeError):
        fastsimdjson.minify("\ud800")


def test_input_types():
    doc = b'{ "a" : [1, 2] }'
    expect = b'{"a":[1,2]}'
    assert fastsimdjson.minify(doc) == expect
    assert fastsimdjson.minify(bytearray(doc)) == expect
    assert fastsimdjson.minify(memoryview(doc)) == expect
    assert fastsimdjson.minify(doc.decode()) == expect
    # A bytearray can be reused; the result is its own bytes object.
    buf = bytearray(doc)
    got = fastsimdjson.minify(buf)
    buf[:] = b" " * len(buf)
    assert got == expect
    with pytest.raises(TypeError):
        fastsimdjson.minify(1)
    with pytest.raises(TypeError, match="contiguous"):
        fastsimdjson.minify(memoryview(bytearray(doc))[::2])


@pytest.mark.parametrize(
    "doc",
    [b'"', b'"abc', b'{"a": "hi', b'   "unterminated', b'"foo\\"', b'"abc\\', b'[ "ok", "'],
)
def test_unclosed_string(doc):
    with pytest.raises(fastsimdjson.JSONDecodeError, match="never closed") as ei:
        fastsimdjson.minify(doc)
    assert ei.value.pos == 0
    with pytest.raises(ValueError):
        reference_minify(doc)
    # The same bytes as str.
    with pytest.raises(fastsimdjson.JSONDecodeError, match="never closed"):
        fastsimdjson.minify(doc.decode("utf-8", "replace"))


def test_corpus():
    if not FILES:
        pytest.skip("simdjson-data not available")
    for path in FILES:
        data = open(path, "rb").read()
        got = fastsimdjson.minify(data)
        assert got == reference_minify(data)
        assert json.loads(got) == json.loads(data)
