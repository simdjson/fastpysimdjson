"""Tests of json_to_msgpack and msgpack_to_json, against msgpack-python."""
import json
import math
import random
import struct
import threading

import pytest

import fastsimdjson

msgpack = pytest.importorskip("msgpack")


def random_string(rng):
    n = rng.choice([0, 1, 31, 32, 255, 256, 70000]) if rng.random() < 0.05 else rng.randint(0, 12)
    return "".join(rng.choices('ab "\\/\n\t\x01\x7fé€\U0001f600', k=n))


def container_size(rng, depth):
    # Sizes around the header boundaries (15, 16), near the top only.
    return rng.choice([0, 1, 15, 16, 17, 3]) if depth < 2 else rng.randint(0, 3)


def random_value(rng, depth=0):
    r = rng.random()
    if depth < 5 and r < 0.15:
        return [random_value(rng, depth + 1) for _ in range(container_size(rng, depth))]
    if depth < 5 and r < 0.3:
        return {random_string(rng): random_value(rng, depth + 1)
                for _ in range(container_size(rng, depth))}
    if r < 0.5:
        return random_string(rng)
    if r < 0.65:
        return rng.randint(-2 ** 63, 2 ** 64 - 1)
    if r < 0.75:
        return rng.randint(-40, 300)
    if r < 0.9:
        x = struct.unpack("<d", struct.pack("<Q", rng.getrandbits(64)))[0]
        return x if math.isfinite(x) else rng.random()
    return rng.choice([True, False, None])


def test_same_bytes_as_msgpack():
    # The most compact encodings: what msgpack.packb gives for json.loads.
    rng = random.Random(1)
    for _ in range(2000):
        value = random_value(rng)
        text = json.dumps(value, ensure_ascii=rng.random() < 0.5)
        assert fastsimdjson.json_to_msgpack(text) == msgpack.packb(json.loads(text))


def test_round_trip():
    rng = random.Random(2)
    for _ in range(2000):
        value = random_value(rng)
        out = fastsimdjson.msgpack_to_json(msgpack.packb(value))
        assert isinstance(out, bytes)
        assert json.loads(out) == value


@pytest.mark.parametrize("value, text", [
    (1.0, b"1.0"), (-0.0, b"-0.0"), (1e16, b"1e+16"), (1e15, b"1000000000000000.0"),
    (0.1, b"0.1"), (2 ** 64 - 1, b"18446744073709551615"), (-2 ** 63, b"-9223372036854775808"),
    ("\x00\"\\", b'"\\u0000\\"\\\\"'), ({}, b"{}"), ([], b"[]"), (None, b"null"),
])
def test_values(value, text):
    assert fastsimdjson.msgpack_to_json(msgpack.packb(value)) == text


def test_inputs():
    text = '{"k": ["v", 1]}'
    expected = msgpack.packb(json.loads(text))
    for data in (text, text.encode(), bytearray(text.encode()), memoryview(text.encode())):
        assert fastsimdjson.json_to_msgpack(data) == expected
    for data in (expected, bytearray(expected), memoryview(expected)):
        assert fastsimdjson.msgpack_to_json(data) == b'{"k":["v",1]}'
    with pytest.raises(TypeError):
        fastsimdjson.msgpack_to_json("not bytes")
    with pytest.raises(TypeError):
        fastsimdjson.json_to_msgpack(12)
    # Inputs of all lengths (ending anywhere on their page): read in place
    # when the padding stays on the page, copied otherwise.
    for n in range(1, 400):
        doc = ("[" + "1," * n + '"' + "x" * (n % 7) + '"]').encode()
        assert fastsimdjson.json_to_msgpack(doc) == msgpack.packb(json.loads(doc))


@pytest.mark.parametrize("threads", [None, 1, 3, 8])
def test_ndjson(threads):
    rng = random.Random(4)
    values = [random_value(rng) for _ in range(3000)]
    text = "\n".join(json.dumps(v) for v in values) + "\n"
    expected = b"".join(msgpack.packb(v) for v in values)
    kw = {} if threads == 1 else {"threads": threads}
    assert fastsimdjson.json_to_msgpack(text, ndjson=True, block_size=4096, **kw) == expected
    back = fastsimdjson.msgpack_to_json(expected, stream=True, block_size=4096, **kw)
    assert [json.loads(line) for line in back.splitlines()] == values


def test_non_json_values():
    data = msgpack.packb({1: b"\x00\xff", None: [b""], 0.5: True, "s": bytearray(b"abc")})
    assert json.loads(fastsimdjson.msgpack_to_json(data)) == {
        "1": "AP8=", "null": [""], "0.5": True, "s": "YWJj"}
    ext = msgpack.packb([msgpack.ExtType(5, b"xyz"), 1])
    with pytest.raises(ValueError, match="extension"):
        fastsimdjson.msgpack_to_json(ext)
    assert fastsimdjson.msgpack_to_json(ext, ext_as_null=True) == b"[null,1]"
    with pytest.raises(ValueError, match="binary"):
        fastsimdjson.msgpack_to_json(msgpack.packb(b"a"), binary_as_base64=False)
    with pytest.raises(ValueError, match="UTF-8"):
        fastsimdjson.msgpack_to_json(b"\xa2\xff\xfe")
    assert fastsimdjson.msgpack_to_json(b"\xa2\xff\xfe", validate_utf8=False) == b'"\xff\xfe"'


def test_errors():
    for bad in (b"", b"[1,", b"[1]]", b'{"a":}', b"nul", b"1 2"):
        with pytest.raises(ValueError):
            fastsimdjson.json_to_msgpack(bad)
    for bad in (b"", b"\xc1", b"\x92\xc0", b"\xdb\xff\xff\xff\xff", b"\xc0\xc0"):
        with pytest.raises(ValueError):
            fastsimdjson.msgpack_to_json(bad)
    with pytest.raises(ValueError, match="document 1"):
        fastsimdjson.json_to_msgpack(b"[1]\n[2,\n", ndjson=True)
    with pytest.raises(ValueError):
        fastsimdjson.json_to_msgpack(b"[1]", threads=-1)
    with pytest.raises(ValueError):
        fastsimdjson.json_to_msgpack(b"[1]", block_size=0)
    with pytest.raises(TypeError):
        fastsimdjson.json_to_msgpack(b"[1]", True)  # ndjson is keyword-only


def test_large():
    # Outputs of all sizes (the result is a bytes object that grows).
    for n in (0, 1, 1000, 100000):
        value = {"k": ["x" * 40] * n}
        packed = fastsimdjson.json_to_msgpack(json.dumps(value))
        assert packed == msgpack.packb(value)
        assert json.loads(fastsimdjson.msgpack_to_json(packed)) == value


def test_concurrent():
    # Python threads at once (the GIL is released while we convert).
    rng = random.Random(5)
    docs = [json.dumps([random_value(rng) for _ in range(50)]) for _ in range(40)]
    expected = [msgpack.packb(json.loads(d)) for d in docs]
    back = [fastsimdjson.msgpack_to_json(e) for e in expected]  # checked by test_round_trip
    errors = []

    def work():
        for _ in range(10):
            for d, e, b in zip(docs, expected, back):
                if fastsimdjson.json_to_msgpack(d) != e or fastsimdjson.msgpack_to_json(e) != b:
                    errors.append(d)

    threads = [threading.Thread(target=work) for _ in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert not errors
