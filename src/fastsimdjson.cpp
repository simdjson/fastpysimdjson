// Fast Python binding for simdjson: parse JSON into native Python objects.
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <cstdint>
#include <cstring>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "simdjson.h"
#include "simdutf.h"

#if PY_VERSION_HEX >= 0x030D0000
// No longer declared in the public headers since 3.13, but still exported.
extern "C" int _PyDict_SetItem_KnownHash(PyObject *mp, PyObject *key,
                                         PyObject *item, Py_hash_t hash);
#endif

#ifdef _MSC_VER
#define FSJ_ALWAYS_INLINE __forceinline
#define FSJ_NOINLINE __declspec(noinline)
#else
#define FSJ_ALWAYS_INLINE __attribute__((always_inline)) inline
#define FSJ_NOINLINE __attribute__((noinline))
#endif

namespace {

using simdjson::dom::parser;

PyObject *JSONDecodeError = nullptr;
PyObject *json_JSONDecodeError = nullptr;
PyObject *json_loads = nullptr;
size_t g_page_size = 4096;

// ---------------------------------------------------------------------------
// Key cache: direct-mapped cache of short ASCII keys. Keys in JSON documents
// repeat a lot; reusing the same str object (with its cached hash) avoids an
// allocation and a hash computation per key. The cache is per thread: a
// process-global cache would race once the GIL is off.
// ---------------------------------------------------------------------------
constexpr size_t KEY_CACHE_SIZE = 2048; // power of two
constexpr size_t KEY_CACHE_MAX_LEN = 64;

// For keys of up to 16 bytes, (len, head, tail) identifies the key exactly;
// longer keys also compare the middle bytes.
struct KeyCacheEntry {
  PyObject *key = nullptr;
  uint64_t len = 0;
  uint64_t head = 0; // first 8 bytes (zero-padded)
  uint64_t tail = 0; // last 8 bytes (0 if len <= 8)
};

void clear_cache(KeyCacheEntry *cache) {
  for (size_t i = 0; i < KEY_CACHE_SIZE; i++) {
    Py_XDECREF(cache[i].key);
    cache[i] = KeyCacheEntry{};
  }
}

// True when this thread may DECREF. A thread-local destructor can run after
// the thread state is gone, or during interpreter finalization; leaking the
// cached strings is safer than touching them then.
bool caches_can_decref() {
#if PY_VERSION_HEX >= 0x030D0000
  return Py_IsInitialized() && !Py_IsFinalizing() &&
         PyThreadState_GetUnchecked() != nullptr;
#else
  return Py_IsInitialized() && PyGILState_GetThisThreadState() != nullptr;
#endif
}

// One simdjson parser per thread, plus that thread's key and string caches.
// The parser is large once a document has been parsed, so it is a pointer
// and release() can delete it.
struct ThreadParser {
  parser *ptr = nullptr;
  KeyCacheEntry key_cache[KEY_CACHE_SIZE]{};
  KeyCacheEntry value_cache[KEY_CACHE_SIZE]{};
  ~ThreadParser() {
    delete ptr;
    ptr = nullptr;
    if (caches_can_decref()) {
      clear_cache(key_cache);
      clear_cache(value_cache);
    }
  }
};
thread_local ThreadParser g_thread_parser;

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

inline PyObject *make_key(KeyCacheEntry *cache, const char *s, size_t len) {
  if (len > KEY_CACHE_MAX_LEN) {
    PyObject *u = make_str(s, len);
    if (u != nullptr && PyObject_Hash(u) == -1) {
      Py_DECREF(u);
      return nullptr;
    }
    return u;
  }
  KeyWords w(s, len);
  // `cache` is this thread's table. A thread_local load per key is visible
  // on documents that are mostly objects.
  KeyCacheEntry &entry = cache[w.slot(len)];
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

// Short ASCII string values repeat often ("en", "false", ...): cache them.
inline PyObject *make_value_str(KeyCacheEntry *cache, const char *s, size_t len) {
  if (len > 16) {
    return make_str(s, len);
  }
  KeyWords w(s, len);
  if (((w.head | w.tail) & 0x8080808080808080ULL) != 0) {
    return make_str(s, len);
  }
  KeyCacheEntry &entry = cache[w.slot(len)];
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
  KeyCacheEntry *keys;
  KeyCacheEntry *values;

  inline const char *str_at(uint64_t word, size_t *len) const {
    size_t idx = size_t(word & simdjson::internal::JSON_VALUE_MASK);
    uint32_t l;
    memcpy(&l, strings + idx, sizeof(l));
    *len = l;
    return reinterpret_cast<const char *>(strings + idx + sizeof(uint32_t));
  }

  // Converts the value at tape[i]; on return, i points past it. Scalars are
  // handled inline so that array/object loops avoid a call per element.
  FSJ_ALWAYS_INLINE PyObject *build(size_t &i) {
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
      return make_value_str(values, s, len);
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

  FSJ_NOINLINE PyObject *build_array(size_t &i, uint64_t word) {
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

  FSJ_NOINLINE PyObject *build_object(size_t &i, uint64_t word) {
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
        PyObject *key = make_key(keys, s, len);
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

// simdjson rejected the document. json.loads decides the outcome: return its
// value when it accepts the text. Raise only when it also fails.
// JSONDecodeError is re-raised as our subclass so the message, document, and
// byte offset match. Any other exception (UnicodeDecodeError, RecursionError)
// propagates unchanged.
PyObject *fallback_loads(PyObject *original) {
  PyObject *arg = original;
  PyObject *bytes_copy = nullptr;
  if (PyMemoryView_Check(original)) {
    // json.loads does not accept memoryview.
    bytes_copy = PyBytes_FromObject(original);
    if (bytes_copy == nullptr) {
      return nullptr;
    }
    arg = bytes_copy;
  }
  PyObject *parsed = PyObject_CallFunctionObjArgs(json_loads, arg, nullptr);
  Py_XDECREF(bytes_copy);
  if (parsed != nullptr || !PyErr_ExceptionMatches(json_JSONDecodeError)) {
    return parsed;
  }
  PyObject *typ = nullptr;
  PyObject *val = nullptr;
  PyObject *tb = nullptr;
  PyErr_Fetch(&typ, &val, &tb);
  PyErr_NormalizeException(&typ, &val, &tb);
  if (val == nullptr) {
    PyErr_Restore(typ, val, tb);
    return nullptr;
  }
  PyObject *msg = PyObject_GetAttrString(val, "msg");
  PyObject *doc = PyObject_GetAttrString(val, "doc");
  PyObject *pos_obj = PyObject_GetAttrString(val, "pos");
  if (msg == nullptr || doc == nullptr || pos_obj == nullptr) {
    PyErr_Clear();
    Py_XDECREF(msg);
    Py_XDECREF(doc);
    Py_XDECREF(pos_obj);
    PyErr_Restore(typ, val, tb);
    return nullptr;
  }
  Py_ssize_t pos = PyLong_AsSsize_t(pos_obj);
  Py_DECREF(pos_obj);
  if (pos == -1 && PyErr_Occurred()) {
    PyErr_Clear();
    Py_DECREF(msg);
    Py_DECREF(doc);
    PyErr_Restore(typ, val, tb);
    return nullptr;
  }
  PyObject *exc = PyObject_CallFunction(JSONDecodeError, "OOn", msg, doc, pos);
  Py_DECREF(msg);
  Py_DECREF(doc);
  Py_XDECREF(typ);
  Py_XDECREF(tb);
  Py_DECREF(val);
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

// The padded parse reads SIMDJSON_PADDING bytes past the last byte. That is
// safe when the read stays on the same page. Otherwise the unpadded DOM entry
// point is used: it does not copy the document and does not read past len.
bool needs_unpadded(const char *buf, size_t len) {
  return len == 0 ||
         ((reinterpret_cast<uintptr_t>(buf + len - 1) % g_page_size) +
              simdjson::SIMDJSON_PADDING >=
          g_page_size);
}

simdjson::error_code parse_input(const char *buf, size_t len) {
  if (needs_unpadded(buf, len)) {
    return g_thread_parser.ptr->parse_unpadded(buf, len).error();
  }
  return g_thread_parser.ptr->parse(buf, len, false).error();
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

void release_caches() {
  clear_cache(g_thread_parser.key_cache);
  clear_cache(g_thread_parser.value_cache);
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
  simdjson::error_code err = parse_input(buf, len);
  if (err) {
    // Drop a huge parser before the json.loads rescan allocates again.
    release_large_parser();
    return fallback_loads(original);
  }
  Builder b{g_thread_parser.ptr->doc.tape.get(),
            g_thread_parser.ptr->doc.string_buf.get(),
            g_thread_parser.key_cache, g_thread_parser.value_cache};
  size_t i = 1; // skip the root entry
  // Suspend the cyclic GC while building: the new containers cannot form
  // cycles, and collections triggered by the many allocations are wasted work.
  // PyGC_Disable is process-global, so free-threaded builds leave GC alone.
#ifndef Py_GIL_DISABLED
  int gc_was_enabled = PyGC_Disable();
#endif
  PyObject *res = b.build(i);
#ifndef Py_GIL_DISABLED
  if (gc_was_enabled) {
    PyGC_Enable();
  }
#endif
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
  if (PyByteArray_Check(arg) || PyMemoryView_Check(arg)) {
    if (PyMemoryView_Check(arg) &&
        !PyBuffer_IsContiguous(PyMemoryView_GET_BUFFER(arg), 'C')) {
      PyErr_SetString(PyExc_TypeError, "memoryview must be contiguous");
      return nullptr;
    }
#ifdef Py_GIL_DISABLED
    // No GIL: another thread can resize the bytearray or retarget the view
    // while it is read. Copy, then parse the copy. bytes and str are immutable.
    PyObject *copy = PyBytes_FromObject(arg);
    if (copy == nullptr) {
      return nullptr;
    }
    PyObject *res = parse_buffer(PyBytes_AS_STRING(copy),
                                 size_t(PyBytes_GET_SIZE(copy)), arg);
    Py_DECREF(copy);
    return res;
#else
    if (PyByteArray_Check(arg)) {
      return parse_buffer(PyByteArray_AS_STRING(arg),
                          size_t(PyByteArray_GET_SIZE(arg)), arg);
    }
    Py_buffer *view = PyMemoryView_GET_BUFFER(arg);
    return parse_buffer(static_cast<const char *>(view->buf),
                        size_t(view->len), arg);
#endif
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
  auto err = parse_input(buf, len);
  release_large_parser();
  return PyLong_FromLong(long(err));
}

PyObject *release(PyObject *, PyObject *) {
  release_parser();
  release_caches();
  Py_RETURN_NONE;
}

PyMethodDef methods[] = {
    {"_parse_only", parse_only, METH_O, "Parse without building objects."},
    {"loads", loads, METH_O, "Deserialize JSON to Python objects."},
    {"release", release, METH_NOARGS,
     "Release the simdjson parser and string caches retained by this thread."},
    {nullptr, nullptr, 0, nullptr},
};

// Multi-phase init so the module can declare that it does not need the GIL.
// Exception types are process-global, so subinterpreters stay unsupported.
int exec_fastsimdjson(PyObject *module) {
  if (json_loads == nullptr) {
    PyObject *json = PyImport_ImportModule("json");
    if (json == nullptr) {
      return -1;
    }
    json_JSONDecodeError = PyObject_GetAttrString(json, "JSONDecodeError");
    json_loads = PyObject_GetAttrString(json, "loads");
    Py_DECREF(json);
    if (json_JSONDecodeError == nullptr || json_loads == nullptr) {
      Py_CLEAR(json_JSONDecodeError);
      Py_CLEAR(json_loads);
      return -1;
    }
  }
  if (JSONDecodeError == nullptr) {
    // Subclass json.JSONDecodeError (itself a ValueError) for drop-in use.
    JSONDecodeError = PyErr_NewException("fastsimdjson.JSONDecodeError",
                                         json_JSONDecodeError, nullptr);
    if (JSONDecodeError == nullptr) {
      return -1;
    }
  }
  return PyModule_AddObjectRef(module, "JSONDecodeError", JSONDecodeError);
}

PyModuleDef_Slot module_slots[] = {
    {Py_mod_exec, reinterpret_cast<void *>(exec_fastsimdjson)},
#if PY_VERSION_HEX >= 0x030C0000
    {Py_mod_multiple_interpreters, Py_MOD_MULTIPLE_INTERPRETERS_NOT_SUPPORTED},
#endif
#if PY_VERSION_HEX >= 0x030D0000
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
#endif
    {0, nullptr},
};

PyModuleDef module_def = {
    PyModuleDef_HEAD_INIT, "fastsimdjson", "Fast JSON parsing with simdjson.",
    0, methods, module_slots,
};

} // namespace

PyMODINIT_FUNC PyInit_fastsimdjson(void) {
#ifdef _WIN32
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  long ps = long(si.dwPageSize);
#else
  long ps = sysconf(_SC_PAGESIZE);
#endif
  if (ps > 0) {
    g_page_size = size_t(ps);
  }
  return PyModuleDef_Init(&module_def);
}
