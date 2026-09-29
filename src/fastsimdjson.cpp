// Fast Python binding for simdjson: parse JSON into native Python objects.
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <cstdint>
#include <cstring>
#include <unistd.h>

#include "simdjson.h"
#include "simdutf.h"

#if PY_VERSION_HEX >= 0x030D0000
// No longer declared in the public headers since 3.13, but still exported.
extern "C" int _PyDict_SetItem_KnownHash(PyObject *mp, PyObject *key,
                                         PyObject *item, Py_hash_t hash);
#endif

namespace {

using simdjson::dom::parser;

PyObject *JSONDecodeError = nullptr;
PyObject *json_JSONDecodeError = nullptr;
PyObject *json_loads = nullptr;
size_t g_page_size = 4096;

// One simdjson parser per thread. The object is large once a document has
// been parsed, so it is a pointer and release() can delete it.
struct ThreadParser {
  parser *ptr = nullptr;
  ~ThreadParser() {
    delete ptr;
    ptr = nullptr;
  }
};
thread_local ThreadParser g_thread_parser;

// ---------------------------------------------------------------------------
// Key cache: direct-mapped cache of short ASCII keys. Keys in JSON documents
// repeat a lot; reusing the same str object (with its cached hash) avoids an
// allocation and a hash computation per key.
// ---------------------------------------------------------------------------
constexpr size_t KEY_CACHE_SIZE = 2048; // power of two
constexpr size_t KEY_CACHE_MAX_LEN = 64;

// For keys of up to 16 bytes, (len, head, tail) identifies the key exactly;
// longer keys also compare the middle bytes.
struct KeyCacheEntry {
  PyObject *key;
  uint64_t len;
  uint64_t head; // first 8 bytes (zero-padded)
  uint64_t tail; // last 8 bytes (0 if len <= 8)
};
KeyCacheEntry key_cache[KEY_CACHE_SIZE];

inline uint64_t load_u64(const char *p) {
  uint64_t v;
  memcpy(&v, p, 8);
  return v;
}

// The simdjson string buffer is padded, so reading 8 bytes past the start of
// a short string is safe; bytes beyond the string are masked off.
struct KeyWords {
  uint64_t head, tail;
  KeyWords(const char *s, size_t len) {
    if (len >= 8) {
      head = load_u64(s);
      tail = load_u64(s + len - 8);
    } else {
      head = len == 0 ? 0 : load_u64(s) & (~uint64_t(0) >> (64 - len * 8));
      tail = 0;
    }
  }
  inline size_t slot(size_t len) const {
    uint64_t h = (head ^ (tail * 0x9E3779B97F4A7C15ULL) ^ len) * 0xFF51AFD7ED558CCDULL;
    return size_t(h >> 40) & (KEY_CACHE_SIZE - 1);
  }
};

inline bool is_ascii(const char *s, size_t len) {
  size_t i = 0;
  uint64_t acc = 0;
  for (; i + 8 <= len; i += 8) {
    acc |= load_u64(s + i);
  }
  for (; i < len; i++) {
    acc |= uint8_t(s[i]);
  }
  return (acc & 0x8080808080808080ULL) == 0;
}

// Decode non-ASCII UTF-8 that simdjson has already validated.
PyObject *decode_utf8(const char *str, size_t len) {
  const uint8_t *s = reinterpret_cast<const uint8_t *>(str);
  size_t nchars = 0;
  uint8_t maxlead = 0;
  for (size_t i = 0; i < len; i++) {
    uint8_t c = s[i];
    nchars += (c & 0xC0) != 0x80;
    maxlead = c > maxlead ? c : maxlead;
  }
  Py_UCS4 maxchar = maxlead >= 0xF0 ? 0x10FFFF : maxlead >= 0xC4 ? 0xFFFF : 0xFF;
  PyObject *u = PyUnicode_New(Py_ssize_t(nchars), maxchar);
  if (u == nullptr) {
    return nullptr;
  }
  const int kind = PyUnicode_KIND(u);
  if (len >= 64) {
    // Long strings: simdutf's SIMD transcoders win despite dispatch overhead.
    size_t n;
    if (kind == PyUnicode_1BYTE_KIND) {
      n = simdutf::convert_valid_utf8_to_latin1(
          str, len, reinterpret_cast<char *>(PyUnicode_1BYTE_DATA(u)));
    } else if (kind == PyUnicode_2BYTE_KIND) {
      n = simdutf::convert_valid_utf8_to_utf16(
          str, len, reinterpret_cast<char16_t *>(PyUnicode_2BYTE_DATA(u)));
    } else {
      n = simdutf::convert_valid_utf8_to_utf32(
          str, len, reinterpret_cast<char32_t *>(PyUnicode_4BYTE_DATA(u)));
    }
    (void)n;
    return u;
  }
  auto decode = [&](auto *out) {
    const uint8_t *p = s, *e = s + len;
    while (p < e) {
      uint32_t c = *p;
      if (c < 0x80) {
        p++;
      } else if (c < 0xE0) {
        c = ((c & 0x1F) << 6) | (p[1] & 0x3F);
        p += 2;
      } else if (c < 0xF0) {
        c = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        p += 3;
      } else {
        c = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) |
            (p[3] & 0x3F);
        p += 4;
      }
      *out++ = decltype(+*out)(c);
    }
  };
  if (kind == PyUnicode_1BYTE_KIND) {
    decode(PyUnicode_1BYTE_DATA(u));
  } else if (kind == PyUnicode_2BYTE_KIND) {
    decode(PyUnicode_2BYTE_DATA(u));
  } else {
    decode(PyUnicode_4BYTE_DATA(u));
  }
  return u;
}

// Create a str from UTF-8 (already validated by simdjson).
inline PyObject *make_str(const char *s, size_t len) {
  if (is_ascii(s, len)) {
    PyObject *u = PyUnicode_New(Py_ssize_t(len), 127);
    if (u == nullptr) {
      return nullptr;
    }
    memcpy(PyUnicode_1BYTE_DATA(u), s, len);
    return u;
  }
  return decode_utf8(s, len);
}

inline PyObject *make_key(const char *s, size_t len) {
  if (len > KEY_CACHE_MAX_LEN) {
    PyObject *u = make_str(s, len);
    if (u != nullptr && PyObject_Hash(u) == -1) {
      Py_DECREF(u);
      return nullptr;
    }
    return u;
  }
  KeyWords w(s, len);
  KeyCacheEntry &entry = key_cache[w.slot(len)];
  if (entry.len == len && entry.head == w.head && entry.tail == w.tail &&
      entry.key != nullptr &&
      (len <= 16 ||
       memcmp(PyUnicode_1BYTE_DATA(entry.key) + 8, s + 8, len - 16) == 0)) {
    Py_INCREF(entry.key);
    return entry.key;
  }
  bool ascii = is_ascii(s, len);
  PyObject *u;
  if (ascii) {
    u = PyUnicode_New(Py_ssize_t(len), 127);
    if (u == nullptr) {
      return nullptr;
    }
    memcpy(PyUnicode_1BYTE_DATA(u), s, len);
  } else {
    u = decode_utf8(s, len);
    if (u == nullptr) {
      return nullptr;
    }
  }
  // Compute the hash now so that dict insertion (and later lookups) reuse it.
  if (PyObject_Hash(u) == -1) {
    Py_DECREF(u);
    return nullptr;
  }
  if (ascii) { // only ASCII keys are cached (their data is the UTF-8 bytes)
    Py_XDECREF(entry.key);
    Py_INCREF(u);
    entry.key = u;
    entry.len = len;
    entry.head = w.head;
    entry.tail = w.tail;
  }
  return u;
}

KeyCacheEntry value_cache[KEY_CACHE_SIZE];

// Short ASCII string values repeat often ("en", "false", ...): cache them.
inline PyObject *make_value_str(const char *s, size_t len) {
  if (len > 16) {
    return make_str(s, len);
  }
  KeyWords w(s, len);
  if (((w.head | w.tail) & 0x8080808080808080ULL) != 0) {
    return make_str(s, len);
  }
  KeyCacheEntry &entry = value_cache[w.slot(len)];
  if (entry.len == len && entry.head == w.head && entry.tail == w.tail &&
      entry.key != nullptr) {
    Py_INCREF(entry.key);
    return entry.key;
  }
  PyObject *u = PyUnicode_New(Py_ssize_t(len), 127);
  if (u == nullptr) {
    return nullptr;
  }
  memcpy(PyUnicode_1BYTE_DATA(u), s, len);
  Py_XDECREF(entry.key);
  Py_INCREF(u);
  entry.key = u;
  entry.len = len;
  entry.head = w.head;
  entry.tail = w.tail;
  return u;
}

// Integers that do not fit in 64 bits. simdjson stores the raw digit text
// (optional leading '-', then digits) in the string buffer, null-terminated.
PyObject *make_bigint(const char *s, size_t len) {
  char *end = nullptr;
  PyObject *n = PyLong_FromString(s, &end, 10);
  if (n == nullptr) {
    return nullptr;
  }
  if (end != s + len) {
    Py_DECREF(n);
    PyErr_SetString(PyExc_RuntimeError, "invalid big integer on tape");
    return nullptr;
  }
  return n;
}

// ---------------------------------------------------------------------------
// Tape walker
// ---------------------------------------------------------------------------
struct Builder {
  const uint64_t *tape;
  const uint8_t *strings;

  inline const char *str_at(uint64_t word, size_t *len) const {
    size_t idx = size_t(word & simdjson::internal::JSON_VALUE_MASK);
    uint32_t l;
    memcpy(&l, strings + idx, sizeof(l));
    *len = l;
    return reinterpret_cast<const char *>(strings + idx + sizeof(uint32_t));
  }

  // Converts the value at tape[i]; on return, i points past it. Scalars are
  // handled inline so that array/object loops avoid a call per element.
  __attribute__((always_inline)) inline PyObject *build(size_t &i) {
    uint64_t word = tape[i];
    uint8_t type = uint8_t(word >> 56);
    switch (type) {
    case '[':
      return build_array(i, word);
    case '{':
      return build_object(i, word);
    case '"': {
      size_t len;
      const char *s = str_at(word, &len);
      i++;
      return make_value_str(s, len);
    }
    case 'l': {
      int64_t v;
      memcpy(&v, &tape[i + 1], 8);
      i += 2;
      return PyLong_FromLongLong(v);
    }
    case 'u': {
      uint64_t v = tape[i + 1];
      i += 2;
      return PyLong_FromUnsignedLongLong(v);
    }
    case 'd': {
      double v;
      memcpy(&v, &tape[i + 1], 8);
      i += 2;
      return PyFloat_FromDouble(v);
    }
    case 'Z': {
      size_t len;
      const char *s = str_at(word, &len);
      i++;
      return make_bigint(s, len);
    }
    case 't':
      i++;
      Py_RETURN_TRUE;
    case 'f':
      i++;
      Py_RETURN_FALSE;
    case 'n':
      i++;
      Py_RETURN_NONE;
    default:
      PyErr_Format(PyExc_RuntimeError, "unexpected tape entry %d", int(type));
      return nullptr;
    }
  }

  __attribute__((noinline)) PyObject *build_array(size_t &i, uint64_t word) {
    {
      size_t end = size_t(word & 0xFFFFFFFF); // index after the matching ']'
      size_t count = size_t((word >> 32) & simdjson::internal::JSON_COUNT_MASK);
      i++;
      if (count < simdjson::internal::JSON_COUNT_MASK) {
        PyObject *list = PyList_New(Py_ssize_t(count));
        if (list == nullptr) {
          return nullptr;
        }
        for (size_t k = 0; k < count; k++) {
          PyObject *v = build(i);
          if (v == nullptr) {
            Py_DECREF(list);
            return nullptr;
          }
          PyList_SET_ITEM(list, Py_ssize_t(k), v);
        }
        i = end;
        return list;
      }
      // Saturated count: append until the closing bracket.
      PyObject *list = PyList_New(0);
      if (list == nullptr) {
        return nullptr;
      }
      while (i < end - 1) {
        PyObject *v = build(i);
        if (v == nullptr || PyList_Append(list, v) < 0) {
          Py_XDECREF(v);
          Py_DECREF(list);
          return nullptr;
        }
        Py_DECREF(v);
      }
      i = end;
      return list;
    }
  }

  __attribute__((noinline)) PyObject *build_object(size_t &i, uint64_t word) {
    {
      size_t end = size_t(word & 0xFFFFFFFF);
      size_t count = size_t((word >> 32) & simdjson::internal::JSON_COUNT_MASK);
      i++;
      PyObject *dict = count > 5 ? _PyDict_NewPresized(Py_ssize_t(count)) : PyDict_New();
      if (dict == nullptr) {
        return nullptr;
      }
      while (i < end - 1) {
        size_t len;
        const char *s = str_at(tape[i], &len);
        i++;
        PyObject *key = make_key(s, len);
        if (key == nullptr) {
          Py_DECREF(dict);
          return nullptr;
        }
        PyObject *v = build(i);
        if (v == nullptr) {
          Py_DECREF(key);
          Py_DECREF(dict);
          return nullptr;
        }
        int rc = _PyDict_SetItem_KnownHash(
            dict, key, v, reinterpret_cast<PyASCIIObject *>(key)->hash);
        Py_DECREF(key);
        Py_DECREF(v);
        if (rc < 0) {
          Py_DECREF(dict);
          return nullptr;
        }
      }
      i = end;
      return dict;
    }
  }
};

// json.JSONDecodeError takes (msg, doc, pos). Used when json.loads cannot
// supply a position: it accepted the text, or it raised some other exception.
void raise_decode_error(const char *msg, PyObject *original) {
  PyObject *exc =
      PyUnicode_Check(original)
          ? PyObject_CallFunction(JSONDecodeError, "sOi", msg, original, 0)
          : PyObject_CallFunction(JSONDecodeError, "ssi", msg, "", 0);
  if (exc != nullptr) {
    PyErr_SetObject(JSONDecodeError, exc);
    Py_DECREF(exc);
  }
}

// simdjson reports no byte offset. Reparse with json.loads and re-raise its
// JSONDecodeError (message, document, and position) as our subclass. Do not
// return a successful json.loads result: json accepts NaN, Infinity, and
// doubles that overflow, which simdjson rejects.
PyObject *raise_error_with_position(const char *simd_msg, PyObject *original) {
  PyObject *arg = original;
  PyObject *bytes_copy = nullptr;
  if (PyMemoryView_Check(original)) {
    bytes_copy = PyBytes_FromObject(original);
    if (bytes_copy == nullptr) {
      PyErr_Clear();
      raise_decode_error(simd_msg, original);
      return nullptr;
    }
    arg = bytes_copy;
  }
  PyObject *parsed = PyObject_CallFunctionObjArgs(json_loads, arg, nullptr);
  Py_XDECREF(bytes_copy);
  if (parsed != nullptr) {
    Py_DECREF(parsed);
    raise_decode_error(simd_msg, original);
    return nullptr;
  }
  if (!PyErr_ExceptionMatches(json_JSONDecodeError)) {
    PyErr_Clear();
    raise_decode_error(simd_msg, original);
    return nullptr;
  }
  PyObject *typ = nullptr;
  PyObject *val = nullptr;
  PyObject *tb = nullptr;
  PyErr_Fetch(&typ, &val, &tb);
  PyErr_NormalizeException(&typ, &val, &tb);
  PyObject *msg = PyObject_GetAttrString(val, "msg");
  PyObject *doc = PyObject_GetAttrString(val, "doc");
  PyObject *pos_obj = PyObject_GetAttrString(val, "pos");
  Py_XDECREF(typ);
  Py_XDECREF(tb);
  Py_DECREF(val);
  if (msg == nullptr || doc == nullptr || pos_obj == nullptr) {
    PyErr_Clear();
    Py_XDECREF(msg);
    Py_XDECREF(doc);
    Py_XDECREF(pos_obj);
    raise_decode_error(simd_msg, original);
    return nullptr;
  }
  Py_ssize_t pos = PyLong_AsSsize_t(pos_obj);
  Py_DECREF(pos_obj);
  if (pos == -1 && PyErr_Occurred()) {
    PyErr_Clear();
    Py_DECREF(msg);
    Py_DECREF(doc);
    raise_decode_error(simd_msg, original);
    return nullptr;
  }
  PyObject *exc = PyObject_CallFunction(JSONDecodeError, "OOn", msg, doc, pos);
  Py_DECREF(msg);
  Py_DECREF(doc);
  if (exc == nullptr) {
    return nullptr;
  }
  PyErr_SetObject(JSONDecodeError, exc);
  Py_DECREF(exc);
  return nullptr;
}

// Documents larger than this are parsed with a parser that is then released,
// so that one huge document does not pin its buffers for the process lifetime.
constexpr size_t MAX_RETAINED_CAPACITY = size_t(64) << 20;

// True when the SIMDJSON_PADDING bytes after buf are not known to be readable.
bool input_needs_copy(const char *buf, size_t len) {
  return len == 0 ||
         ((reinterpret_cast<uintptr_t>(buf + len - 1) % g_page_size) +
              simdjson::SIMDJSON_PADDING >=
          g_page_size);
}

bool ensure_parser() {
  if (g_thread_parser.ptr != nullptr) {
    return true;
  }
  g_thread_parser.ptr = new (std::nothrow) parser();
  if (g_thread_parser.ptr == nullptr) {
    PyErr_NoMemory();
    return false;
  }
  // Keep integers that do not fit in 64 bits on the tape as digit strings,
  // instead of failing the parse and reprocessing the document.
  g_thread_parser.ptr->number_as_string(true);
  return true;
}

void release_parser() {
  delete g_thread_parser.ptr;
  g_thread_parser.ptr = nullptr;
}

void release_large_parser() {
  if (g_thread_parser.ptr != nullptr &&
      g_thread_parser.ptr->capacity() > MAX_RETAINED_CAPACITY) {
    release_parser();
  }
}

PyObject *parse_buffer(const char *buf, size_t len, PyObject *original) {
  if (!ensure_parser()) {
    return nullptr;
  }
  auto result = g_thread_parser.ptr->parse(buf, len, input_needs_copy(buf, len));
  simdjson::error_code err = result.error();
  if (err) {
    // Drop a huge parser before the json.loads rescan allocates again.
    const char *msg = simdjson::error_message(err);
    release_large_parser();
    return raise_error_with_position(msg, original);
  }
  Builder b{g_thread_parser.ptr->doc.tape.get(),
            g_thread_parser.ptr->doc.string_buf.get()};
  size_t i = 1; // skip the root entry
  // Suspend the cyclic GC while building: the new containers cannot form
  // cycles, and collections triggered by the many allocations are wasted work.
  int gc_was_enabled = PyGC_Disable();
  PyObject *res = b.build(i);
  if (gc_was_enabled) {
    PyGC_Enable();
  }
  release_large_parser();
  return res;
}

PyObject *loads(PyObject *, PyObject *arg) {
  if (PyBytes_Check(arg)) {
    return parse_buffer(PyBytes_AS_STRING(arg), size_t(PyBytes_GET_SIZE(arg)),
                        arg);
  }
  if (PyUnicode_Check(arg)) {
    Py_ssize_t len;
    const char *s = PyUnicode_AsUTF8AndSize(arg, &len);
    if (s == nullptr) {
      return nullptr;
    }
    return parse_buffer(s, size_t(len), arg);
  }
  if (PyByteArray_Check(arg)) {
    return parse_buffer(PyByteArray_AS_STRING(arg),
                        size_t(PyByteArray_GET_SIZE(arg)), arg);
  }
  if (PyMemoryView_Check(arg)) {
    Py_buffer *view = PyMemoryView_GET_BUFFER(arg);
    if (!PyBuffer_IsContiguous(view, 'C')) {
      PyErr_SetString(PyExc_TypeError, "memoryview must be contiguous");
      return nullptr;
    }
    return parse_buffer(static_cast<const char *>(view->buf),
                        size_t(view->len), arg);
  }
  PyErr_Format(PyExc_TypeError,
               "Input must be bytes, bytearray, memoryview, or str, not %.200s",
               Py_TYPE(arg)->tp_name);
  return nullptr;
}

// Benchmarking helper: run simdjson without building Python objects.
PyObject *parse_only(PyObject *, PyObject *arg) {
  if (!PyBytes_Check(arg)) {
    PyErr_SetString(PyExc_TypeError, "bytes expected");
    return nullptr;
  }
  if (!ensure_parser()) {
    return nullptr;
  }
  const char *buf = PyBytes_AS_STRING(arg);
  size_t len = size_t(PyBytes_GET_SIZE(arg));
  auto err =
      g_thread_parser.ptr->parse(buf, len, input_needs_copy(buf, len)).error();
  release_large_parser();
  return PyLong_FromLong(long(err));
}

PyObject *release(PyObject *, PyObject *) {
  release_parser();
  Py_RETURN_NONE;
}

PyMethodDef methods[] = {
    {"_parse_only", parse_only, METH_O, "Parse without building objects."},
    {"loads", loads, METH_O, "Deserialize JSON to Python objects."},
    {"release", release, METH_NOARGS,
     "Release the simdjson parser retained by this thread."},
    {nullptr, nullptr, 0, nullptr},
};

PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT, "fastsimdjson", "Fast JSON parsing with simdjson.",
    -1, methods,
};

} // namespace

PyMODINIT_FUNC PyInit_fastsimdjson(void) {
  long ps = sysconf(_SC_PAGESIZE);
  if (ps > 0) {
    g_page_size = size_t(ps);
  }
  PyObject *m = PyModule_Create(&module_def);
  if (m == nullptr) {
    return nullptr;
  }
  PyObject *json = PyImport_ImportModule("json");
  if (json == nullptr) {
    Py_DECREF(m);
    return nullptr;
  }
  json_JSONDecodeError = PyObject_GetAttrString(json, "JSONDecodeError");
  if (json_JSONDecodeError == nullptr) {
    Py_DECREF(json);
    Py_DECREF(m);
    return nullptr;
  }
  json_loads = PyObject_GetAttrString(json, "loads");
  Py_DECREF(json);
  if (json_loads == nullptr) {
    Py_DECREF(m);
    return nullptr;
  }
  // Subclass json.JSONDecodeError (itself a ValueError) for drop-in use.
  JSONDecodeError = PyErr_NewException("fastsimdjson.JSONDecodeError",
                                       json_JSONDecodeError, nullptr);
  if (JSONDecodeError == nullptr) {
    Py_DECREF(m);
    return nullptr;
  }
  Py_INCREF(JSONDecodeError);
  if (PyModule_AddObject(m, "JSONDecodeError", JSONDecodeError) < 0) {
    Py_DECREF(JSONDecodeError);
    Py_DECREF(m);
    return nullptr;
  }
  return m;
}
