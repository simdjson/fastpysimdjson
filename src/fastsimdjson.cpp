// Fast Python binding for simdjson: parse JSON into native Python objects.
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

// simdjson.cpp is compiled here (a unity build) so that dumps can call its
// float-to-decimal routine, dtoa_impl::dragonbox, directly.
#include "simdjson.h"
#include "simdjson.cpp"
#include "simdutf.h"

#ifdef _MSC_VER
#include <intrin.h>
inline int ctz64(uint64_t x) {
  unsigned long i;
  _BitScanForward64(&i, x);
  return int(i);
}
#else
inline int ctz64(uint64_t x) { return __builtin_ctzll(x); }
#endif

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define FSJ_SSE2 1
#elif defined(__ARM_NEON) || defined(_M_ARM64)
#include <arm_neon.h>
#define FSJ_NEON 1
#endif

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

using simdjson::dom::document;
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
// and release() can delete it. `spare` is a document released by parse()
// (see the lazy API below), kept for the next parse() on this thread.
// dumpb's dict keys, as written (quoted and escaped), by object identity.
// The cache holds a reference to each key, so an entry's object cannot be
// freed and its address reused while it is in the cache.
constexpr size_t ENCODED_KEY_CACHE_SIZE = 1024; // power of two
constexpr size_t ENCODED_KEY_MAX = 64;          // bytes of text
struct EncodedKey {
  PyObject *key = nullptr;
  uint32_t len = 0;
  char text[ENCODED_KEY_MAX];
};

struct ThreadParser {
  parser *ptr = nullptr;
  simdjson::dom::document *spare = nullptr;
  KeyCacheEntry key_cache[KEY_CACHE_SIZE]{};
  KeyCacheEntry value_cache[KEY_CACHE_SIZE]{};
  EncodedKey *encoded_keys = nullptr; // allocated on first use by dumpb
  void clear_encoded_keys() {
    if (encoded_keys != nullptr) {
      for (size_t i = 0; i < ENCODED_KEY_CACHE_SIZE; i++) {
        Py_XDECREF(encoded_keys[i].key);
      }
      delete[] encoded_keys;
      encoded_keys = nullptr;
    }
  }
  ~ThreadParser() {
    delete ptr;
    ptr = nullptr;
    delete spare;
    spare = nullptr;
    if (caches_can_decref()) {
      clear_cache(key_cache);
      clear_cache(value_cache);
      clear_encoded_keys();
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

// A str from ASCII bytes.
inline PyObject *new_ascii(const char *s, size_t len) {
  PyObject *u = PyUnicode_New(Py_ssize_t(len), 127);
  if (u != nullptr && len != 0) {
    memcpy(PyUnicode_1BYTE_DATA(u), s, len);
  }
  return u;
}

// Create a str from UTF-8 (already validated by simdjson).
inline PyObject *make_str(const char *s, size_t len) {
  return is_ascii(s, len) ? new_ascii(s, len) : decode_utf8(s, len);
}

inline void cache_store(KeyCacheEntry &entry, PyObject *u, size_t len, const KeyWords &w) {
  Py_INCREF(u);
  Py_XDECREF(entry.key);
  entry.key = u;
  entry.len = len;
  entry.head = w.head;
  entry.tail = w.tail;
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
  PyObject *u = ascii ? new_ascii(s, len) : decode_utf8(s, len);
  // Compute the hash now so that dict insertion (and later lookups) reuse it.
  if (u == nullptr || PyObject_Hash(u) == -1) {
    Py_XDECREF(u);
    return nullptr;
  }
  if (ascii) { // only ASCII keys are cached (their data is the UTF-8 bytes)
    cache_store(entry, u, len, w);
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
  PyObject *u = new_ascii(s, len);
  if (u != nullptr) {
    cache_store(entry, u, len, w);
  }
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

  // Uses the calling thread's string caches.
  explicit Builder(const document &doc)
      : tape(doc.tape.get()), strings(doc.string_buf.get()),
        keys(g_thread_parser.key_cache), values(g_thread_parser.value_cache) {}

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

  FSJ_NOINLINE PyObject *build_object(size_t &i, uint64_t word) {
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
};

// Replaces a pending json.JSONDecodeError with our subclass, with the same
// message, document and position. Other exceptions are left as they are.
void reraise_decode_error() {
  if (!PyErr_ExceptionMatches(json_JSONDecodeError)) {
    return;
  }
  PyObject *typ, *val, *tb;
  PyErr_Fetch(&typ, &val, &tb);
  PyErr_NormalizeException(&typ, &val, &tb);
  PyObject *msg = PyObject_GetAttrString(val, "msg");
  PyObject *doc = PyObject_GetAttrString(val, "doc");
  PyObject *pos = PyObject_GetAttrString(val, "pos");
  PyObject *exc = msg && doc && pos
                      ? PyObject_CallFunctionObjArgs(JSONDecodeError, msg, doc, pos, nullptr)
                      : nullptr;
  Py_XDECREF(msg);
  Py_XDECREF(doc);
  Py_XDECREF(pos);
  if (exc == nullptr) {
    PyErr_Clear();
    PyErr_Restore(typ, val, tb);
    return;
  }
  Py_XDECREF(typ);
  Py_XDECREF(val);
  Py_XDECREF(tb);
  PyErr_SetObject(JSONDecodeError, exc);
  Py_DECREF(exc);
}

// simdjson rejected the document. json.loads decides the outcome: return its
// value when it accepts the text, or raise its error (JSONDecodeError as our
// subclass; any other exception, such as UnicodeDecodeError, unchanged).
PyObject *fallback_loads(PyObject *original) {
  // json.loads does not accept memoryview.
  PyObject *arg = PyMemoryView_Check(original) ? PyBytes_FromObject(original)
                                               : Py_NewRef(original);
  if (arg == nullptr) {
    return nullptr;
  }
  PyObject *parsed = PyObject_CallOneArg(json_loads, arg);
  Py_DECREF(arg);
  if (parsed == nullptr) {
    reraise_decode_error();
  }
  return parsed;
}

// Documents larger than this are parsed with a parser that is then released,
// so that one huge document does not pin its buffers for the process lifetime.
constexpr size_t MAX_RETAINED_CAPACITY = size_t(64) << 20;

// The padded parse reads SIMDJSON_PADDING bytes past the last byte. That is
// safe when the read stays on the same page. Otherwise the unpadded DOM entry
// point is used: it does not copy the document and does not read past len.
bool needs_unpadded(const char *buf, size_t len) {
#if defined(__SANITIZE_ADDRESS__)
  // AddressSanitizer reports the in-page read past the end of the buffer.
  return true;
#endif
  return len == 0 ||
         ((reinterpret_cast<uintptr_t>(buf + len - 1) % g_page_size) +
              simdjson::SIMDJSON_PADDING >=
          g_page_size);
}

// Parses buf with this thread's parser into d (the parser's own document
// for loads, a document of its own for parse).
simdjson::error_code parse_into(document &d, const char *buf, size_t len) {
  const uint8_t *u = reinterpret_cast<const uint8_t *>(buf);
  return needs_unpadded(buf, len)
             ? g_thread_parser.ptr->parse_into_document_unpadded(d, u, len).error()
             : g_thread_parser.ptr->parse_into_document(d, u, len, false).error();
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

// Suspend the cyclic GC while building: the new containers cannot form
// cycles, and collections triggered by the many allocations are wasted work.
// PyGC_Disable is process-global, so free-threaded builds leave GC alone.
PyObject *build_with_gc_paused(const document &doc, size_t i) {
#ifndef Py_GIL_DISABLED
  int gc_was_enabled = PyGC_Disable();
#endif
  PyObject *res = Builder(doc).build(i);
#ifndef Py_GIL_DISABLED
  if (gc_was_enabled) {
    PyGC_Enable();
  }
#endif
  return res;
}

PyObject *parse_buffer(const char *buf, size_t len, PyObject *original) {
  if (!ensure_parser()) {
    return nullptr;
  }
  if (parse_into(g_thread_parser.ptr->doc, buf, len)) {
    // Drop a huge parser before the json.loads rescan allocates again.
    release_large_parser();
    return fallback_loads(original);
  }
  PyObject *res = build_with_gc_paused(g_thread_parser.ptr->doc, 1); // skip the root entry
  release_large_parser();
  return res;
}

// Calls Parse on the bytes of arg (bytes, str, bytearray or memoryview).
template <PyObject *(*Parse)(const char *, size_t, PyObject *)>
PyObject *with_input(PyObject *arg) {
  if (PyBytes_Check(arg)) {
    return Parse(PyBytes_AS_STRING(arg), size_t(PyBytes_GET_SIZE(arg)), arg);
  }
  if (PyUnicode_Check(arg)) {
    Py_ssize_t len;
    const char *s = PyUnicode_AsUTF8AndSize(arg, &len);
    if (s == nullptr) {
      return nullptr;
    }
    return Parse(s, size_t(len), arg);
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
    PyObject *res = Parse(PyBytes_AS_STRING(copy), size_t(PyBytes_GET_SIZE(copy)), arg);
    Py_DECREF(copy);
    return res;
#else
    if (PyByteArray_Check(arg)) {
      return Parse(PyByteArray_AS_STRING(arg), size_t(PyByteArray_GET_SIZE(arg)), arg);
    }
    Py_buffer *view = PyMemoryView_GET_BUFFER(arg);
    return Parse(static_cast<const char *>(view->buf), size_t(view->len), arg);
#endif
  }
  PyErr_Format(PyExc_TypeError,
               "Input must be bytes, bytearray, memoryview, or str, not %.200s",
               Py_TYPE(arg)->tp_name);
  return nullptr;
}

PyObject *loads(PyObject *, PyObject *arg) {
  return with_input<parse_buffer>(arg);
}

// ---------------------------------------------------------------------------
// Lazy API: parse() keeps the simdjson document and returns proxies over its
// tape (Object, Array). Python objects are created only for the values that
// are accessed. Each document owns its tape and string buffer, so it stays
// valid while the thread's parser goes on to parse other documents.
// ---------------------------------------------------------------------------
#if PY_VERSION_HEX < 0x030D0000
#define Py_BEGIN_CRITICAL_SECTION(op) {
#define Py_END_CRITICAL_SECTION() }
#endif

PyTypeObject *DocumentType = nullptr;
PyTypeObject *ObjectType = nullptr;
PyTypeObject *ArrayType = nullptr;
PyTypeObject *IterType = nullptr;

struct DocumentObject {
  PyObject_HEAD
  document *doc;
};

// fastsimdjson.Object and fastsimdjson.Array.
struct ProxyObject {
  PyObject_HEAD
  DocumentObject *owner;
  size_t start; // tape index of '{' or '['
  // Array: (index << 32) | tape position of the element last reached by
  // subscripting (0: none), so that a loop over a[i] is linear.
  std::atomic<uint64_t> cursor;
};

enum IterKind { ITER_ELEMENTS, ITER_KEYS, ITER_VALUES, ITER_ITEMS };

struct IterObject {
  PyObject_HEAD
  DocumentObject *owner;
  size_t pos; // tape index of the next element or key
  size_t end; // tape index of the closing bracket
  IterKind kind;
};

// Keep one released document per thread for the next parse(), unless it is
// large or the interpreter is going away.
void give_back(document *d) {
  if (g_thread_parser.spare == nullptr && caches_can_decref() &&
      d->capacity() <= MAX_RETAINED_CAPACITY) {
    g_thread_parser.spare = d;
  } else {
    delete d;
  }
}

inline const uint64_t *tape_of(DocumentObject *owner) {
  return owner->doc->tape.get();
}

// Tape index of the value that follows the one at i.
inline size_t skip(const uint64_t *tape, size_t i) {
  uint64_t word = tape[i];
  switch (uint8_t(word >> 56)) {
  case '[':
  case '{':
    return size_t(word & 0xFFFFFFFF);
  case 'l':
  case 'u':
  case 'd':
    return i + 2;
  default:
    return i + 1;
  }
}

// Tape index of the closing bracket of the container at start.
inline size_t close_of(const uint64_t *tape, size_t start) {
  return size_t(tape[start] & 0xFFFFFFFF) - 1;
}

size_t count_of(const uint64_t *tape, size_t start) {
  uint64_t word = tape[start];
  size_t count = size_t((word >> 32) & simdjson::internal::JSON_COUNT_MASK);
  if (count < simdjson::internal::JSON_COUNT_MASK) {
    return count;
  }
  // Saturated count: walk the container.
  bool is_object = uint8_t(word >> 56) == '{';
  size_t end = close_of(tape, start);
  size_t n = 0;
  for (size_t i = start + 1; i < end; n++) {
    i = skip(tape, is_object ? i + 1 : i);
  }
  return n;
}

PyObject *new_proxy(PyTypeObject *type, DocumentObject *owner, size_t start) {
  ProxyObject *p = PyObject_New(ProxyObject, type);
  if (p == nullptr) {
    return nullptr;
  }
  Py_INCREF(owner);
  p->owner = owner;
  p->start = start;
  new (&p->cursor) std::atomic<uint64_t>(0);
  return reinterpret_cast<PyObject *>(p);
}

// The value at tape index i: a proxy for a container, a Python object
// otherwise.
PyObject *value_at(DocumentObject *owner, size_t i) {
  switch (uint8_t(tape_of(owner)[i] >> 56)) {
  case '{':
    return new_proxy(ObjectType, owner, i);
  case '[':
    return new_proxy(ArrayType, owner, i);
  default:
    return Builder(*owner->doc).build(i);
  }
}

// Tape index of the value of the first field named key, or 0.
size_t find_key(DocumentObject *owner, size_t start, const char *key,
                size_t len) {
  const uint64_t *tape = tape_of(owner);
  const uint8_t *strings = owner->doc->string_buf.get();
  size_t end = close_of(tape, start);
  for (size_t i = start + 1; i < end; i = skip(tape, i + 1)) {
    size_t idx = size_t(tape[i] & simdjson::internal::JSON_VALUE_MASK);
    uint32_t l;
    memcpy(&l, strings + idx, sizeof(l));
    if (l == len && memcmp(strings + idx + sizeof(uint32_t), key, len) == 0) {
      return i + 1;
    }
  }
  return 0;
}

// Tape index of element k of an array, starting from element j at tape
// index i, or 0 when the array, which closes at `end`, is shorter.
size_t walk_to(const uint64_t *tape, size_t i, size_t j, size_t k, size_t end) {
  for (; j < k && i < end; j++) {
    i = skip(tape, i);
  }
  return i < end ? i : 0;
}

// Tape index of element k of the array a, or 0.
size_t find_index(ProxyObject *a, size_t k) {
  const uint64_t *tape = tape_of(a->owner);
  uint64_t c = a->cursor.load(std::memory_order_relaxed);
  bool resume = c != 0 && size_t(c >> 32) <= k;
  size_t i = walk_to(tape, resume ? size_t(c & 0xFFFFFFFF) : a->start + 1,
                     resume ? size_t(c >> 32) : 0, k, close_of(tape, a->start));
  if (i != 0) {
    a->cursor.store((uint64_t(k) << 32) | i, std::memory_order_relaxed);
  }
  return i;
}

// Follows a JSON Pointer (RFC 6901) from the value at tape index i.
PyObject *at_pointer(DocumentObject *owner, size_t i, PyObject *arg) {
  Py_ssize_t plen;
  const char *p = PyUnicode_Check(arg) ? PyUnicode_AsUTF8AndSize(arg, &plen)
                                       : nullptr;
  if (p == nullptr) {
    if (!PyErr_Occurred()) {
      PyErr_SetString(PyExc_TypeError, "JSON pointer must be a str");
    }
    return nullptr;
  }
  if (plen > 0 && p[0] != '/') {
    PyErr_Format(PyExc_ValueError, "invalid JSON pointer: %R", arg);
    return nullptr;
  }
  const uint64_t *tape = tape_of(owner);
  const char *end = p + plen;
  const char *cur = p;
  std::string token;
  while (cur < end) {
    cur++; // '/'
    const char *stop = static_cast<const char *>(memchr(cur, '/', size_t(end - cur)));
    if (stop == nullptr) {
      stop = end;
    }
    token.assign(cur, size_t(stop - cur));
    cur = stop;
    for (size_t t = token.find('~'); t != std::string::npos; t = token.find('~', t + 1)) {
      if (t + 1 >= token.size() || (token[t + 1] != '0' && token[t + 1] != '1')) {
        PyErr_Format(PyExc_ValueError, "invalid JSON pointer: %R", arg);
        return nullptr;
      }
      token.replace(t, 2, token[t + 1] == '0' ? "~" : "/");
    }
    uint8_t type = uint8_t(tape[i] >> 56);
    if (type == '{') {
      size_t v = find_key(owner, i, token.data(), token.size());
      if (v == 0) {
        PyObject *k = PyUnicode_FromStringAndSize(token.data(), Py_ssize_t(token.size()));
        if (k != nullptr) {
          PyErr_SetObject(PyExc_KeyError, k);
          Py_DECREF(k);
        }
        return nullptr;
      }
      i = v;
    } else if (type == '[') {
      bool digits = !token.empty() && (token.size() == 1 || token[0] != '0') &&
                    token.size() <= 10;
      size_t k = 0;
      for (char ch : token) {
        digits = digits && ch >= '0' && ch <= '9';
        k = k * 10 + size_t(ch - '0');
      }
      if (!digits) {
        PyErr_Format(PyExc_IndexError, "invalid array index in JSON pointer: %R", arg);
        return nullptr;
      }
      i = walk_to(tape, i + 1, 0, k, close_of(tape, i));
      if (i == 0) {
        PyErr_Format(PyExc_IndexError, "array index out of range in JSON pointer: %R", arg);
        return nullptr;
      }
    } else {
      PyErr_Format(PyExc_ValueError, "JSON pointer %R goes through a scalar", arg);
      return nullptr;
    }
  }
  return value_at(owner, i);
}

// --- Document ---------------------------------------------------------------

void document_dealloc(PyObject *self) {
  PyTypeObject *tp = Py_TYPE(self);
  give_back(reinterpret_cast<DocumentObject *>(self)->doc);
  PyObject_Free(self);
  Py_DECREF(tp);
}

PyType_Slot document_slots[] = {
    {Py_tp_dealloc, reinterpret_cast<void *>(document_dealloc)},
    {0, nullptr},
};

PyType_Spec document_spec = {
    "fastsimdjson._Document", sizeof(DocumentObject), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION, document_slots,
};

// --- Object and Array -------------------------------------------------------

inline ProxyObject *proxy(PyObject *self) {
  return reinterpret_cast<ProxyObject *>(self);
}

void proxy_dealloc(PyObject *self) {
  PyTypeObject *tp = Py_TYPE(self);
  Py_DECREF(proxy(self)->owner);
  PyObject_Free(self);
  Py_DECREF(tp);
}

Py_ssize_t proxy_length(PyObject *self) {
  return Py_ssize_t(count_of(tape_of(proxy(self)->owner), proxy(self)->start));
}

PyObject *proxy_at_pointer(PyObject *self, PyObject *arg) {
  return at_pointer(proxy(self)->owner, proxy(self)->start, arg);
}

PyObject *proxy_materialize(PyObject *self, PyObject *) {
  return build_with_gc_paused(*proxy(self)->owner->doc, proxy(self)->start);
}

PyObject *new_iter(PyObject *self, IterKind kind) {
  ProxyObject *p = proxy(self);
  IterObject *it = PyObject_New(IterObject, IterType);
  if (it == nullptr) {
    return nullptr;
  }
  Py_INCREF(p->owner);
  it->owner = p->owner;
  it->pos = p->start + 1;
  it->end = close_of(tape_of(p->owner), p->start);
  it->kind = kind;
  return reinterpret_cast<PyObject *>(it);
}

// Object

// Returns the tape index of the value, 0 if absent, or -1 (as size_t) with
// an exception set.
size_t object_lookup(PyObject *self, PyObject *key) {
  if (!PyUnicode_Check(key)) {
    return 0;
  }
  Py_ssize_t len;
  const char *s = PyUnicode_AsUTF8AndSize(key, &len);
  if (s == nullptr) {
    // Lone surrogates cannot be encoded, and cannot be keys of a document.
    if (!PyErr_ExceptionMatches(PyExc_UnicodeEncodeError)) {
      return size_t(-1);
    }
    PyErr_Clear();
    return 0;
  }
  return find_key(proxy(self)->owner, proxy(self)->start, s, size_t(len));
}

PyObject *object_subscript(PyObject *self, PyObject *key) {
  size_t v = object_lookup(self, key);
  if (v == size_t(-1)) {
    return nullptr;
  }
  if (v == 0) {
    PyErr_SetObject(PyExc_KeyError, key);
    return nullptr;
  }
  return value_at(proxy(self)->owner, v);
}

int object_contains(PyObject *self, PyObject *key) {
  size_t v = object_lookup(self, key);
  return v == size_t(-1) ? -1 : v != 0;
}

PyObject *object_get(PyObject *self, PyObject *const *args, Py_ssize_t nargs) {
  if (nargs < 1 || nargs > 2) {
    PyErr_SetString(PyExc_TypeError, "get expected 1 or 2 arguments");
    return nullptr;
  }
  size_t v = object_lookup(self, args[0]);
  if (v == size_t(-1)) {
    return nullptr;
  }
  if (v == 0) {
    PyObject *dflt = nargs == 2 ? args[1] : Py_None;
    Py_INCREF(dflt);
    return dflt;
  }
  return value_at(proxy(self)->owner, v);
}

PyObject *object_iter(PyObject *self) { return new_iter(self, ITER_KEYS); }
PyObject *object_keys(PyObject *self, PyObject *) { return new_iter(self, ITER_KEYS); }
PyObject *object_values(PyObject *self, PyObject *) { return new_iter(self, ITER_VALUES); }
PyObject *object_items(PyObject *self, PyObject *) { return new_iter(self, ITER_ITEMS); }

PyObject *object_repr(PyObject *self) {
  return PyUnicode_FromFormat("<fastsimdjson.Object with %zd keys>", proxy_length(self));
}

PyMethodDef object_methods[] = {
    {"get", reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(object_get)),
     METH_FASTCALL, "Value of key, or default (None) if there is no such key."},
    {"keys", object_keys, METH_NOARGS, "Iterator over the keys."},
    {"values", object_values, METH_NOARGS, "Iterator over the values."},
    {"items", object_items, METH_NOARGS, "Iterator over (key, value) pairs."},
    {"as_dict", proxy_materialize, METH_NOARGS,
     "Convert to a dict, recursively, like loads."},
    {"at_pointer", proxy_at_pointer, METH_O, "Value at a JSON Pointer (RFC 6901)."},
    {nullptr, nullptr, 0, nullptr},
};

PyType_Slot object_slots[] = {
    {Py_tp_dealloc, reinterpret_cast<void *>(proxy_dealloc)},
    {Py_tp_repr, reinterpret_cast<void *>(object_repr)},
    {Py_tp_iter, reinterpret_cast<void *>(object_iter)},
    {Py_tp_methods, object_methods},
    {Py_mp_length, reinterpret_cast<void *>(proxy_length)},
    {Py_mp_subscript, reinterpret_cast<void *>(object_subscript)},
    {Py_sq_contains, reinterpret_cast<void *>(object_contains)},
    {Py_tp_doc, const_cast<char *>(
        "Read-only view of a JSON object. Values are converted on access; "
        "nested objects and arrays are returned as views.")},
    {0, nullptr},
};

PyType_Spec object_spec = {
    "fastsimdjson.Object", sizeof(ProxyObject), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION | Py_TPFLAGS_MAPPING,
    object_slots,
};

// Array

PyObject *array_item(PyObject *self, Py_ssize_t k) {
  Py_ssize_t n = -1;
  if (k < 0) {
    n = proxy_length(self);
    k += n;
  }
  size_t i = k < 0 ? 0 : find_index(proxy(self), size_t(k));
  if (i == 0) {
    PyErr_SetString(PyExc_IndexError, "array index out of range");
    return nullptr;
  }
  return value_at(proxy(self)->owner, i);
}

PyObject *array_subscript(PyObject *self, PyObject *key) {
  if (PySlice_Check(key)) {
    Py_ssize_t start, stop, step;
    if (PySlice_Unpack(key, &start, &stop, &step) < 0) {
      return nullptr;
    }
    Py_ssize_t n = PySlice_AdjustIndices(proxy_length(self), &start, &stop, step);
    PyObject *list = PyList_New(n);
    if (list == nullptr) {
      return nullptr;
    }
    for (Py_ssize_t j = 0, k = start; j < n; j++, k += step) {
      PyObject *v = array_item(self, k);
      if (v == nullptr) {
        Py_DECREF(list);
        return nullptr;
      }
      PyList_SET_ITEM(list, j, v);
    }
    return list;
  }
  Py_ssize_t k = PyNumber_AsSsize_t(key, PyExc_IndexError);
  if (k == -1 && PyErr_Occurred()) {
    return nullptr;
  }
  return array_item(self, k);
}

PyObject *array_iter(PyObject *self) { return new_iter(self, ITER_ELEMENTS); }

PyObject *array_repr(PyObject *self) {
  return PyUnicode_FromFormat("<fastsimdjson.Array with %zd elements>", proxy_length(self));
}

PyMethodDef array_methods[] = {
    {"as_list", proxy_materialize, METH_NOARGS,
     "Convert to a list, recursively, like loads."},
    {"at_pointer", proxy_at_pointer, METH_O, "Value at a JSON Pointer (RFC 6901)."},
    {nullptr, nullptr, 0, nullptr},
};

PyType_Slot array_slots[] = {
    {Py_tp_dealloc, reinterpret_cast<void *>(proxy_dealloc)},
    {Py_tp_repr, reinterpret_cast<void *>(array_repr)},
    {Py_tp_iter, reinterpret_cast<void *>(array_iter)},
    {Py_tp_methods, array_methods},
    {Py_mp_length, reinterpret_cast<void *>(proxy_length)},
    {Py_mp_subscript, reinterpret_cast<void *>(array_subscript)},
    {Py_sq_length, reinterpret_cast<void *>(proxy_length)},
    {Py_sq_item, reinterpret_cast<void *>(array_item)},
    {Py_tp_doc, const_cast<char *>(
        "Read-only view of a JSON array. Elements are converted on access; "
        "nested objects and arrays are returned as views.")},
    {0, nullptr},
};

PyType_Spec array_spec = {
    "fastsimdjson.Array", sizeof(ProxyObject), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION | Py_TPFLAGS_SEQUENCE,
    array_slots,
};

// --- Iterator ---------------------------------------------------------------

void iter_dealloc(PyObject *self) {
  PyTypeObject *tp = Py_TYPE(self);
  Py_DECREF(reinterpret_cast<IterObject *>(self)->owner);
  PyObject_Free(self);
  Py_DECREF(tp);
}

PyObject *iter_next(PyObject *self) {
  IterObject *it = reinterpret_cast<IterObject *>(self);
  const uint64_t *tape = tape_of(it->owner);
  size_t pos;
  // A free-threaded build may share an iterator between threads: claim the
  // element and advance in one step.
  Py_BEGIN_CRITICAL_SECTION(self);
  pos = it->pos;
  if (pos < it->end) {
    it->pos = skip(tape, it->kind == ITER_ELEMENTS ? pos : pos + 1);
  }
  Py_END_CRITICAL_SECTION();
  if (pos >= it->end) {
    return nullptr;
  }
  switch (it->kind) {
  case ITER_ELEMENTS:
    return value_at(it->owner, pos);
  case ITER_VALUES:
    return value_at(it->owner, pos + 1);
  default:
    break;
  }
  size_t len;
  const char *s = Builder(*it->owner->doc).str_at(tape[pos], &len);
  PyObject *key = make_key(g_thread_parser.key_cache, s, len);
  if (key == nullptr || it->kind == ITER_KEYS) {
    return key;
  }
  PyObject *value = value_at(it->owner, pos + 1);
  if (value == nullptr) {
    Py_DECREF(key);
    return nullptr;
  }
  PyObject *pair = PyTuple_New(2);
  if (pair == nullptr) {
    Py_DECREF(key);
    Py_DECREF(value);
    return nullptr;
  }
  PyTuple_SET_ITEM(pair, 0, key);
  PyTuple_SET_ITEM(pair, 1, value);
  return pair;
}

PyType_Slot iter_slots[] = {
    {Py_tp_dealloc, reinterpret_cast<void *>(iter_dealloc)},
    {Py_tp_iter, reinterpret_cast<void *>(PyObject_SelfIter)},
    {Py_tp_iternext, reinterpret_cast<void *>(iter_next)},
    {0, nullptr},
};

PyType_Spec iter_spec = {
    "fastsimdjson._Iterator", sizeof(IterObject), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION, iter_slots,
};

// --- parse() ----------------------------------------------------------------

// The root of document d, which the returned view takes over.
PyObject *view_of(document *d) {
  DocumentObject *owner = PyObject_New(DocumentObject, DocumentType);
  if (owner == nullptr) {
    give_back(d);
    return nullptr;
  }
  owner->doc = d;
  PyObject *root = value_at(owner, 1); // skip the root entry
  Py_DECREF(owner);
  return root;
}

PyObject *parse_lazy_buffer(const char *buf, size_t len, PyObject *original) {
  if (!ensure_parser()) {
    return nullptr;
  }
  document *d = g_thread_parser.spare;
  g_thread_parser.spare = nullptr;
  if (d == nullptr) {
    d = new (std::nothrow) document();
    if (d == nullptr) {
      return PyErr_NoMemory();
    }
  }
  simdjson::error_code err = parse_into(*d, buf, len);
  release_large_parser();
  if (err) {
    give_back(d);
    return fallback_loads(original);
  }
  return view_of(d);
}

PyObject *parse(PyObject *, PyObject *arg) {
  return with_input<parse_lazy_buffer>(arg);
}

// ---------------------------------------------------------------------------
// dumps: Python objects to JSON text, with the output of json.dumps. The
// encoder handles the usual types and options; anything else (a cls
// argument, skipkeys, a circular reference, NaN with allow_nan=False, a key
// or an object it cannot encode) is handed to json.dumps, which produces the
// result or raises the same exception that json would raise.
// ---------------------------------------------------------------------------
enum EncodeStatus { ENCODE_OK = 0, ENCODE_ERROR = -1, ENCODE_FALLBACK = 1 };

// A new reference to item i of a list, or nullptr past its end: the list may
// shrink while we encode it (default may change it, or another thread).
inline PyObject *list_item(PyObject *list, Py_ssize_t i) {
  if (i >= PyList_GET_SIZE(list)) {
    return nullptr; // checked first: an IndexError per list would be costly
  }
#if PY_VERSION_HEX >= 0x030D0000
  PyObject *item = PyList_GetItemRef(list, i);
  if (item == nullptr) {
    PyErr_Clear();
  }
  return item;
#else
  return Py_NewRef(PyList_GET_ITEM(list, i));
#endif
}

const char HEX_DIGITS[] = "0123456789abcdef";

// Growable output buffer. Callers reserve the worst case for a value, then
// write without checks. Allocation failure throws std::bad_alloc, caught in
// dumps (no Python frame is unwound: the encoder only throws from its
// own frames).
struct OutBuf {
  char *buf = nullptr;
  size_t len = 0;
  size_t cap = 0;
  OutBuf() = default;
  OutBuf(const OutBuf &) = delete;
  OutBuf &operator=(const OutBuf &) = delete;
  ~OutBuf() { free(buf); }
  FSJ_NOINLINE void grow(size_t n) {
    size_t c = std::max(cap * 2, len + n + 1024);
    char *p = static_cast<char *>(realloc(buf, c));
    if (p == nullptr) {
      throw std::bad_alloc();
    }
    buf = p;
    cap = c;
  }
  // Pointer to the end of the output, with room for n more bytes.
  inline char *reserve(size_t n) {
    if (len + n > cap) {
      grow(n);
    }
    return buf + len;
  }
  inline void push_back(char c) {
    *reserve(1) = c;
    len++;
  }
  inline void append(const char *s, size_t n) {
    memcpy(reserve(n), s, n);
    len += n;
  }
  inline void append(const char *s) { append(s, strlen(s)); }
  inline void append(const std::string &s) { append(s.data(), s.size()); }
  inline void append(size_t n, char c) {
    memset(reserve(n), c, n);
    len += n;
  }
  size_t size() const { return len; }
  const char *data() const { return buf; }
};

// A finite double as Python's repr writes it, or as orjson does. dragonbox
// gives the shortest digits that round-trip, the same digits as repr
// (checked on millions of values). repr writes them in fixed notation when
// the decimal exponent is in (-4, 16], and as d.ddde+XX otherwise (at least
// two exponent digits); orjson uses fixed notation from one more decade
// down, (-5, 16], and does not pad the exponent (1e-6).
template <bool Orjson>
void append_float(OutBuf &out, double v) {
  // Digits are copied in fixed blocks of 17 (dragonbox writes at most 17),
  // which compile to a few moves: reserve room for the overshoot.
  char *p = out.reserve(64);
  char *start = p;
  if (std::signbit(v)) {
    *p++ = '-';
    v = -v;
  }
  if (v == 0) {
    memcpy(p, "0.0", 3);
    out.len += size_t(p + 3 - start);
    return;
  }
  char d[simdjson::internal::to_chars_buffer_size + 24];
  int nd, e10;
  simdjson::internal::dtoa_impl::dragonbox(d, nd, e10, v);
  int decpt = nd + e10; // v = 0.d * 10^decpt
  if (decpt > (Orjson ? -5 : -4) && decpt <= 16) {
    if (decpt <= 0) {
      memcpy(p, "0.0000", 8); // "0." and up to four zeros
      p += 2 - decpt;
      memcpy(p, d, 17);
      p += nd;
    } else if (decpt >= nd) {
      memcpy(p, d, 17);
      p += nd;
      memset(p, '0', 16);
      p += decpt - nd;
      memcpy(p, ".0", 2);
      p += 2;
    } else {
      memcpy(p, d, 17);
      p += decpt;
      *p++ = '.';
      memcpy(p, d + decpt, 17);
      p += nd - decpt;
    }
  } else {
    *p++ = d[0];
    if (nd > 1) {
      *p++ = '.';
      memcpy(p, d + 1, 16);
      p += nd - 1;
    }
    int e = decpt - 1;
    *p++ = 'e';
    *p++ = e < 0 ? '-' : '+';
    if (e < 0) {
      e = -e;
    }
    if (e >= 100) {
      *p++ = char('0' + e / 100);
    }
    if (e >= 10 || !Orjson) {
      *p++ = char('0' + (e / 10) % 10);
    }
    *p++ = char('0' + e % 10);
  }
  out.len += size_t(p - start);
}

const char DIGIT_PAIRS[] =
    "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
    "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

// Decimal digits of v, two at a time.
inline void append_u64(OutBuf &out, uint64_t v) {
  char buf[24];
  char *p = buf + sizeof(buf);
  while (v >= 100) {
    p -= 2;
    memcpy(p, DIGIT_PAIRS + 2 * (v % 100), 2);
    v /= 100;
  }
  if (v >= 10) {
    p -= 2;
    memcpy(p, DIGIT_PAIRS + 2 * v, 2);
  } else {
    *--p = char('0' + v);
  }
  out.append(p, size_t(buf + sizeof(buf) - p));
}

inline void append_i64(OutBuf &out, long long v) {
  if (v < 0) {
    out.push_back('-');
    append_u64(out, uint64_t(0) - uint64_t(v));
  } else {
    append_u64(out, uint64_t(v));
  }
}

// Writes the JSON escape of c at p (at most 12 bytes); returns the new end.
inline char *write_escaped(char *p, uint32_t c) {
  char short_form = 0;
  switch (c) {
  case '"': short_form = '"'; break;
  case '\\': short_form = '\\'; break;
  case '\b': short_form = 'b'; break;
  case '\f': short_form = 'f'; break;
  case '\n': short_form = 'n'; break;
  case '\r': short_form = 'r'; break;
  case '\t': short_form = 't'; break;
  default: break;
  }
  if (short_form != 0) {
    p[0] = '\\';
    p[1] = short_form;
    return p + 2;
  }
  if (c >= 0x10000) {
    c -= 0x10000;
    p = write_escaped(p, 0xD800 | (c >> 10));
    return write_escaped(p, 0xDC00 | (c & 0x3FF));
  }
  p[0] = '\\';
  p[1] = 'u';
  p[2] = HEX_DIGITS[(c >> 12) & 0xF];
  p[3] = HEX_DIGITS[(c >> 8) & 0xF];
  p[4] = HEX_DIGITS[(c >> 4) & 0xF];
  p[5] = HEX_DIGITS[c & 0xF];
  return p + 6;
}

// The ASCII characters that need an escape: control characters, '"', '\',
// and DEL when escape_del (json with ensure_ascii escapes all but ' '..'~').
inline bool byte_needs_escape(uint8_t c, bool escape_del) {
  return c < 0x20 || c == '"' || c == '\\' || (c == 0x7F && escape_del);
}

// Bit mask of the bytes of the 16 at s that need an escape; bytes >= 0x80
// (UTF-8 sequences) never do.
#if FSJ_SSE2
inline uint32_t escape_mask16(const char *s, bool escape_del) {
  __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(s));
  __m128i ctrl = _mm_cmpeq_epi8(_mm_max_epu8(v, _mm_set1_epi8(0x1F)), _mm_set1_epi8(0x1F));
  __m128i m = _mm_or_si128(ctrl, _mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8('"')),
                                              _mm_cmpeq_epi8(v, _mm_set1_epi8('\\'))));
  if (escape_del) {
    m = _mm_or_si128(m, _mm_cmpeq_epi8(v, _mm_set1_epi8(0x7F)));
  }
  return uint32_t(_mm_movemask_epi8(m));
}
#elif FSJ_NEON
inline uint64_t escape_mask16(const char *s, bool escape_del) {
  uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t *>(s));
  uint8x16_t m = vorrq_u8(vcleq_u8(v, vdupq_n_u8(0x1F)),
                          vorrq_u8(vceqq_u8(v, vdupq_n_u8('"')), vceqq_u8(v, vdupq_n_u8('\\'))));
  if (escape_del) {
    m = vorrq_u8(m, vceqq_u8(v, vdupq_n_u8(0x7F)));
  }
  // Four bits per byte.
  return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(m), 4)), 0);
}
#endif

// Copies s[0, len) to p, escaping the ASCII characters that need it; bytes
// >= 0x80 (UTF-8 sequences) are copied as they are. p must have room for
// 6 * len + 16 bytes. Returns the new end.
inline char *escape_into(char *p, const char *s, size_t len, bool escape_del) {
  size_t i = 0;
#if FSJ_SSE2 || FSJ_NEON
  while (i + 16 <= len) {
    auto mask = escape_mask16(s + i, escape_del);
    memcpy(p, s + i, 16); // speculative: overwritten below if needed
    if (mask == 0) {
      p += 16;
      i += 16;
      continue;
    }
#if FSJ_SSE2
    size_t k = size_t(ctz64(mask));
#else
    size_t k = size_t(ctz64(mask)) / 4;
#endif
    p += k;
    i += k;
    p = write_escaped(p, uint8_t(s[i]));
    i++;
  }
  if (i < len) {
    // The tail (under 16 bytes), checked in a buffer padded with spaces.
    char tail[16];
    memset(tail, ' ', 16);
    memcpy(tail, s + i, len - i);
    if (escape_mask16(tail, escape_del) == 0) {
      memcpy(p, tail, 16); // the caller reserved the room
      return p + (len - i);
    }
  }
#endif
  for (; i < len; i++) {
    uint8_t c = uint8_t(s[i]);
    if (byte_needs_escape(c, escape_del)) {
      p = write_escaped(p, c);
    } else {
      *p++ = char(c);
    }
  }
  return p;
}

struct Encoder {
  OutBuf out;
  bool ensure_ascii = true;
  bool ascii_output = true; // ensure_ascii, and ASCII separators and indent
  bool allow_nan = true;
  bool sort_keys = false;
  PyObject *default_fn = nullptr; // borrowed
  std::string item_sep = ", ";
  std::string key_sep = ": ";
  bool has_indent = false;
  std::string indent;
  int level = 0;
  std::vector<PyObject *> path; // containers being encoded

  void newline_indent() {
    out.push_back('\n');
    for (int i = 0; i < level; i++) {
      out.append(indent);
    }
  }

  // What precedes each element of a container, and what closes it.
  void begin_item(bool first) {
    if (!first) {
      out.append(item_sep);
    }
    if (has_indent) {
      newline_indent();
    }
  }

  void close(char bracket) {
    level--;
    if (has_indent) {
      newline_indent();
    }
    out.push_back(bracket);
  }

  inline bool ascii_needs_escape(uint8_t c) const {
    return byte_needs_escape(c, ensure_ascii);
  }

  // Writes the code units s[0, len) with every non-ASCII character escaped
  // (ensure_ascii). p must have room for 12 * len bytes.
  template <typename T>
  inline char *write_ascii_escaped(char *p, const T *s, size_t len) const {
    for (size_t i = 0; i < len; i++) {
      uint32_t c = s[i];
      if (c < 0x80 && !ascii_needs_escape(uint8_t(c))) {
        *p++ = char(c);
      } else {
        p = write_escaped(p, c);
      }
    }
    return p;
  }

  // The leaf encoders are out of line, which keeps the frames of the
  // recursive encode small (json.dumps handles deep nesting, so must we).
  FSJ_NOINLINE int encode_str(PyObject *s) {
    size_t len = size_t(PyUnicode_GET_LENGTH(s));
    if (PyUnicode_IS_ASCII(s)) {
      char *p = out.reserve(6 * len + 18);
      *p++ = '"';
      p = escape_into(p, static_cast<const char *>(PyUnicode_DATA(s)), len, ensure_ascii);
      *p++ = '"';
      out.len = size_t(p - out.buf);
      return ENCODE_OK;
    }
    int kind = PyUnicode_KIND(s);
    const void *data = PyUnicode_DATA(s);
    if (ensure_ascii) {
      char *p = out.reserve(12 * len + 2);
      *p++ = '"';
      if (kind == PyUnicode_1BYTE_KIND) {
        p = write_ascii_escaped(p, static_cast<const uint8_t *>(data), len);
      } else if (kind == PyUnicode_2BYTE_KIND) {
        p = write_ascii_escaped(p, static_cast<const uint16_t *>(data), len);
      } else {
        p = write_ascii_escaped(p, static_cast<const uint32_t *>(data), len);
      }
      *p++ = '"';
      out.len = size_t(p - out.buf);
      return ENCODE_OK;
    }
    // UTF-8 output: transcode with simdutf, then escape the few ASCII
    // characters that need it. simdutf rejects lone surrogates, which json
    // keeps in its str output: those strings go to json.dumps.
    size_t utf8_max = (kind == PyUnicode_1BYTE_KIND ? 2 : kind == PyUnicode_2BYTE_KIND ? 3 : 4) * len;
    char *p = out.reserve(utf8_max + 6 * utf8_max + 18);
    char *tmp = p + 6 * utf8_max + 18; // transcode past the room for the escaped copy
    size_t n;
    if (kind == PyUnicode_1BYTE_KIND) {
      n = simdutf::convert_latin1_to_utf8(static_cast<const char *>(data), len, tmp);
    } else if (kind == PyUnicode_2BYTE_KIND) {
      // A str may hold a high and a low surrogate as two code points; json
      // keeps them apart, but simdutf would join them into one character.
      const uint16_t *u = static_cast<const uint16_t *>(data);
      bool surrogate = false;
      for (size_t i = 0; i < len; i++) {
        surrogate |= (u[i] & 0xF800) == 0xD800;
      }
      if (surrogate) {
        return ENCODE_FALLBACK;
      }
      n = simdutf::convert_utf16_to_utf8(static_cast<const char16_t *>(data), len, tmp);
    } else {
      n = simdutf::convert_utf32_to_utf8(static_cast<const char32_t *>(data), len, tmp);
    }
    if (n == 0 && len != 0) {
      return ENCODE_FALLBACK; // a lone surrogate cannot be UTF-8
    }
    *p++ = '"';
    p = escape_into(p, tmp, n, ensure_ascii);
    *p++ = '"';
    out.len = size_t(p - out.buf);
    return ENCODE_OK;
  }

  FSJ_NOINLINE int encode_int(PyObject *o) {
    int overflow = 0;
    long long v = PyLong_AsLongLongAndOverflow(o, &overflow);
    if (overflow == 0) {
      if (v == -1 && PyErr_Occurred()) {
        return ENCODE_ERROR;
      }
      append_i64(out, v);
      return ENCODE_OK;
    }
    // int.__repr__, as json does (an int subclass may override __repr__).
    PyObject *r = PyLong_Type.tp_repr(o);
    if (r == nullptr) {
      return ENCODE_ERROR;
    }
    Py_ssize_t n;
    const char *p = PyUnicode_AsUTF8AndSize(r, &n);
    if (p != nullptr) {
      out.append(p, size_t(n));
    }
    Py_DECREF(r);
    return p == nullptr ? ENCODE_ERROR : ENCODE_OK;
  }

  FSJ_NOINLINE int encode_float(double v) {
    if (std::isfinite(v)) {
      append_float<false>(out, v);
      return ENCODE_OK;
    }
    if (!allow_nan) {
      return ENCODE_FALLBACK;
    }
    out.append(std::isnan(v) ? "NaN" : v > 0 ? "Infinity" : "-Infinity");
    return ENCODE_OK;
  }

  bool on_path(PyObject *o) const {
    for (PyObject *p : path) {
      if (p == o) {
        return true;
      }
    }
    return false;
  }

  FSJ_NOINLINE int encode_key(PyObject *key) {
    if (PyUnicode_Check(key)) {
      return encode_str(key);
    }
    if (PyFloat_Check(key)) {
      out.push_back('"');
      int rc = encode_float(PyFloat_AS_DOUBLE(key));
      out.push_back('"');
      return rc;
    }
    if (key == Py_True || key == Py_False || key == Py_None) {
      out.append(key == Py_True ? "\"true\"" : key == Py_False ? "\"false\"" : "\"null\"");
      return ENCODE_OK;
    }
    if (PyLong_Check(key)) {
      out.push_back('"');
      int rc = encode_int(key);
      out.push_back('"');
      return rc;
    }
    return ENCODE_FALLBACK; // json raises TypeError (or skips the key)
  }

  FSJ_NOINLINE int encode_item(PyObject *key, PyObject *value, bool first) {
    begin_item(first);
    int rc = encode_key(key);
    if (rc != ENCODE_OK) {
      return rc;
    }
    out.append(key_sep);
    return encode(value);
  }

  FSJ_NOINLINE int encode_dict(PyObject *o) {
    if (PyDict_GET_SIZE(o) == 0) {
      out.append("{}");
      return ENCODE_OK;
    }
    out.push_back('{');
    level++;
    int rc = ENCODE_OK;
#ifdef Py_GIL_DISABLED
    bool snapshot = true; // another thread may change the dict
#else
    // Python code (default, key comparisons) may change the dict.
    bool snapshot = sort_keys || default_fn != nullptr || !PyDict_CheckExact(o);
#endif
    if (snapshot) {
      PyObject *items = PyDict_CheckExact(o) ? PyDict_Items(o) : PyMapping_Items(o);
      if (items == nullptr || (sort_keys && PyList_Sort(items) < 0)) {
        Py_XDECREF(items);
        return ENCODE_ERROR;
      }
      for (Py_ssize_t i = 0; rc == ENCODE_OK && i < PyList_GET_SIZE(items); i++) {
        PyObject *pair = PyList_GET_ITEM(items, i);
        if (!PyTuple_Check(pair) || PyTuple_GET_SIZE(pair) != 2) {
          rc = ENCODE_FALLBACK;
          break;
        }
        rc = encode_item(PyTuple_GET_ITEM(pair, 0), PyTuple_GET_ITEM(pair, 1), i == 0);
      }
      Py_DECREF(items);
    } else {
      Py_ssize_t pos = 0;
      PyObject *key, *value;
      bool first = true;
      while (rc == ENCODE_OK && PyDict_Next(o, &pos, &key, &value)) {
        Py_INCREF(key);
        Py_INCREF(value);
        rc = encode_item(key, value, first);
        Py_DECREF(key);
        Py_DECREF(value);
        first = false;
      }
    }
    if (rc == ENCODE_OK) {
      close('}');
    }
    return rc;
  }

  FSJ_NOINLINE int encode_list(PyObject *o) {
    bool is_list = PyList_Check(o);
    Py_ssize_t n = is_list ? PyList_GET_SIZE(o) : PyTuple_GET_SIZE(o);
    if (n == 0) {
      out.append("[]");
      return ENCODE_OK;
    }
    out.push_back('[');
    level++;
    for (Py_ssize_t i = 0;; i++) {
      PyObject *item = is_list ? list_item(o, i)
                       : i < n ? Py_NewRef(PyTuple_GET_ITEM(o, i))
                               : nullptr;
      if (item == nullptr) {
        break;
      }
      begin_item(i == 0);
      int rc = encode(item);
      Py_DECREF(item);
      if (rc != ENCODE_OK) {
        return rc;
      }
    }
    close(']');
    return ENCODE_OK;
  }

  int encode(PyObject *o) {
    if (o == Py_None) {
      out.append("null");
      return ENCODE_OK;
    }
    if (o == Py_True) {
      out.append("true");
      return ENCODE_OK;
    }
    if (o == Py_False) {
      out.append("false");
      return ENCODE_OK;
    }
    if (PyUnicode_Check(o)) {
      return encode_str(o);
    }
    if (PyLong_Check(o)) {
      return encode_int(o);
    }
    if (PyFloat_Check(o)) {
      return encode_float(PyFloat_AS_DOUBLE(o));
    }
    bool container = PyList_Check(o) || PyTuple_Check(o) || PyDict_Check(o);
    if (!container && default_fn == nullptr) {
      return ENCODE_FALLBACK; // json raises TypeError
    }
    if (on_path(o)) {
      return ENCODE_FALLBACK; // json raises "Circular reference detected"
    }
    if (Py_EnterRecursiveCall(" while encoding a JSON object")) {
      return ENCODE_ERROR;
    }
    path.push_back(o);
    int rc;
    if (PyDict_Check(o)) {
      rc = encode_dict(o);
    } else if (container) {
      rc = encode_list(o);
    } else {
      PyObject *r = PyObject_CallOneArg(default_fn, o);
      rc = r == nullptr ? ENCODE_ERROR : encode(r);
      Py_XDECREF(r);
    }
    path.pop_back();
    Py_LeaveRecursiveCall();
    return rc;
  }
};

PyObject *json_dumps = nullptr;

// Reads the json.dumps keywords. Returns false when the call should go to
// json.dumps (unknown or unusual arguments); sets an error and returns false
// with *error set on failure.
bool configure(Encoder &e, PyObject *kwargs, bool *error) {
  *error = false;
  if (kwargs == nullptr) {
    return true;
  }
  PyObject *separators = Py_None;
  PyObject *indent = Py_None;
  Py_ssize_t pos = 0;
  PyObject *k, *v;
  while (PyDict_Next(kwargs, &pos, &k, &v)) {
    const char *name = PyUnicode_AsUTF8(k);
    if (name == nullptr) {
      *error = true;
      return false;
    }
    int truth = -2;
    if (strcmp(name, "ensure_ascii") == 0 || strcmp(name, "allow_nan") == 0 ||
        strcmp(name, "sort_keys") == 0 || strcmp(name, "skipkeys") == 0 ||
        strcmp(name, "check_circular") == 0) {
      truth = PyObject_IsTrue(v);
      if (truth < 0) {
        *error = true;
        return false;
      }
    }
    if (strcmp(name, "ensure_ascii") == 0) {
      e.ensure_ascii = truth;
    } else if (strcmp(name, "allow_nan") == 0) {
      e.allow_nan = truth;
    } else if (strcmp(name, "sort_keys") == 0) {
      e.sort_keys = truth;
    } else if (strcmp(name, "skipkeys") == 0) {
      if (truth) {
        return false;
      }
    } else if (strcmp(name, "check_circular") == 0) {
      // Cycles go to json.dumps, which honors the flag.
    } else if (strcmp(name, "default") == 0) {
      if (v != Py_None) {
        e.default_fn = v;
      }
    } else if (strcmp(name, "cls") == 0) {
      if (v != Py_None) {
        return false;
      }
    } else if (strcmp(name, "indent") == 0) {
      indent = v;
    } else if (strcmp(name, "separators") == 0) {
      separators = v;
    } else {
      return false; // json.dumps passes other keywords to cls
    }
  }
  if (indent != Py_None) {
    e.has_indent = true;
    e.item_sep = ",";
    if (PyLong_Check(indent)) {
      long n = PyLong_AsLong(indent);
      if (n == -1 && PyErr_Occurred()) {
        PyErr_Clear();
        return false;
      }
      e.indent.assign(n > 0 ? size_t(n) : 0, ' ');
    } else if (PyUnicode_Check(indent)) {
      Py_ssize_t n;
      const char *s = PyUnicode_AsUTF8AndSize(indent, &n);
      if (s == nullptr) {
        PyErr_Clear();
        return false;
      }
      e.indent.assign(s, size_t(n));
    } else {
      return false;
    }
  }
  if (separators != Py_None) {
    if (!(PyTuple_Check(separators) || PyList_Check(separators)) ||
        PySequence_Fast_GET_SIZE(separators) != 2) {
      return false;
    }
    PyObject *a = PySequence_Fast_GET_ITEM(separators, 0);
    PyObject *b = PySequence_Fast_GET_ITEM(separators, 1);
    if (!PyUnicode_Check(a) || !PyUnicode_Check(b)) {
      return false;
    }
    Py_ssize_t na, nb;
    const char *sa = PyUnicode_AsUTF8AndSize(a, &na);
    const char *sb = PyUnicode_AsUTF8AndSize(b, &nb);
    if (sa == nullptr || sb == nullptr) {
      PyErr_Clear();
      return false;
    }
    e.item_sep.assign(sa, size_t(na));
    e.key_sep.assign(sb, size_t(nb));
  }
  // Separators and indentation are copied as UTF-8, so they decide whether
  // the output can be built as an ASCII string.
  e.ascii_output = e.ensure_ascii;
  for (const std::string *s : {&e.item_sep, &e.key_sep, &e.indent}) {
    for (char c : *s) {
      if (uint8_t(c) >= 0x80) {
        e.ascii_output = false;
      }
    }
  }
  return true;
}

PyObject *dumps(PyObject *, PyObject *args, PyObject *kwargs) {
  Encoder e;
  bool error = false;
  if (PyTuple_GET_SIZE(args) != 1 || !configure(e, kwargs, &error)) {
    return error ? nullptr : PyObject_Call(json_dumps, args, kwargs);
  }
  int rc;
  try {
    rc = e.encode(PyTuple_GET_ITEM(args, 0));
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  }
  if (rc == ENCODE_ERROR) {
    return nullptr;
  }
  if (rc == ENCODE_FALLBACK) {
    return PyObject_Call(json_dumps, args, kwargs);
  }
  return e.ascii_output ? new_ascii(e.out.data(), e.out.size())
                        : PyUnicode_DecodeUTF8(e.out.data(), Py_ssize_t(e.out.size()), nullptr);
}

// ---------------------------------------------------------------------------
// dumpb: the output of orjson.dumps (compact UTF-8 bytes), with its
// arguments (default, option) and errors, for the JSON types: str, int,
// float, bool, None, list, tuple, dict, their subclasses, and enums. The
// other types orjson serializes natively (dataclasses, datetime, UUID,
// numpy) go to default.
// ---------------------------------------------------------------------------
enum : unsigned long {
  OPT_INDENT_2 = 1,
  OPT_NON_STR_KEYS = 4,
  OPT_SORT_KEYS = 32,
  OPT_STRICT_INTEGER = 64,
  OPT_PASSTHROUGH_SUBCLASS = 256,
  OPT_APPEND_NEWLINE = 1024,
  OPT_ALL = 4095, // orjson's options; the others concern types we do not serialize
};
constexpr int ORJSON_MAX_DEPTH = 254;
PyObject *EnumType = nullptr; // enum.Enum

struct OrjsonEncoder {
  OutBuf out;
  PyObject *default_fn = nullptr; // borrowed
  unsigned long opts = 0;
  int depth = 0;         // containers being encoded
  int default_calls = 0; // nested calls to default

  int fail(const char *msg) {
    PyErr_SetString(PyExc_TypeError, msg);
    return -1;
  }

  void newline() {
    size_t n = 1 + 2 * size_t(depth);
    char *p = out.reserve(n);
    p[0] = '\n';
    memset(p + 1, ' ', n - 1);
    out.len += n;
  }

  int encode_str(PyObject *s) {
    Py_ssize_t n;
    const char *p;
    if (PyUnicode_IS_COMPACT_ASCII(s)) {
      p = static_cast<const char *>(PyUnicode_DATA(s));
      n = PyUnicode_GET_LENGTH(s);
    } else {
      p = PyUnicode_AsUTF8AndSize(s, &n); // cached on the str, as orjson does
      if (p == nullptr) {
        PyErr_Clear();
        return fail("str is not valid UTF-8: surrogates not allowed");
      }
    }
    char *w = out.reserve(6 * size_t(n) + 18);
    *w++ = '"';
    w = escape_into(w, p, size_t(n), false);
    *w++ = '"';
    out.len = size_t(w - out.buf);
    return 0;
  }

  int encode_int(PyObject *o) {
    int overflow = 0;
    long long v = PyLong_AsLongLongAndOverflow(o, &overflow);
    if (overflow == 0) {
      if (v == -1 && PyErr_Occurred()) {
        return -1;
      }
      if ((opts & OPT_STRICT_INTEGER) && (v > 9007199254740991LL || v < -9007199254740991LL)) {
        return fail("Integer exceeds 53-bit range");
      }
      append_i64(out, v);
      return 0;
    }
    if (overflow > 0) {
      unsigned long long u = PyLong_AsUnsignedLongLong(o);
      if (u == (unsigned long long)-1 && PyErr_Occurred()) {
        PyErr_Clear();
      } else {
        if (opts & OPT_STRICT_INTEGER) {
          return fail("Integer exceeds 53-bit range");
        }
        append_u64(out, u);
        return 0;
      }
    }
    return fail("Integer exceeds 64-bit range");
  }

  void encode_float(double v) {
    if (std::isfinite(v)) {
      append_float<true>(out, v);
    } else {
      out.append("null", 4);
    }
  }

  // A dict key: a str, or with OPT_NON_STR_KEYS, the text of an int, float,
  // bool, None or enum.
  // An exact str key, from the cache of encoded keys when possible.
  int encode_str_key(PyObject *k) {
    EncodedKey *cache = g_thread_parser.encoded_keys;
    if (cache == nullptr) {
      cache = g_thread_parser.encoded_keys = new (std::nothrow) EncodedKey[ENCODED_KEY_CACHE_SIZE];
      if (cache == nullptr) {
        return encode_str(k);
      }
    }
    EncodedKey &e = cache[(reinterpret_cast<uintptr_t>(k) >> 4) & (ENCODED_KEY_CACHE_SIZE - 1)];
    if (e.key == k) {
      memcpy(out.reserve(ENCODED_KEY_MAX), e.text, ENCODED_KEY_MAX);
      out.len += e.len;
      return 0;
    }
    size_t start = out.len;
    if (encode_str(k) < 0) {
      return -1;
    }
    size_t n = out.len - start;
    if (n <= ENCODED_KEY_MAX) {
      Py_INCREF(k);
      Py_XDECREF(e.key);
      e.key = k;
      e.len = uint32_t(n);
      memcpy(e.text, out.buf + start, n);
    }
    return 0;
  }

  int encode_key(PyObject *k) {
    if (PyUnicode_CheckExact(k)) {
      return encode_str_key(k);
    }
    if (PyUnicode_Check(k) && !(opts & OPT_PASSTHROUGH_SUBCLASS)) {
      return encode_str(k);
    }
    if (!(opts & OPT_NON_STR_KEYS)) {
      return fail("Dict key must be str");
    }
    if (k == Py_True || k == Py_False || k == Py_None) {
      out.append(k == Py_True ? "\"true\"" : k == Py_False ? "\"false\"" : "\"null\"");
      return 0;
    }
    if (PyLong_Check(k) || PyFloat_Check(k)) {
      out.push_back('"');
      int rc = 0;
      if (PyLong_Check(k)) {
        rc = encode_int(k);
      } else {
        encode_float(PyFloat_AS_DOUBLE(k));
      }
      out.push_back('"');
      return rc;
    }
    if (PyObject_IsInstance(k, EnumType) == 1) {
      PyObject *value = PyObject_GetAttrString(k, "value");
      if (value == nullptr) {
        return -1;
      }
      int rc = encode_key(value);
      Py_DECREF(value);
      return rc;
    }
    return fail("Dict key must a type serializable with OPT_NON_STR_KEYS");
  }

  int open(char bracket) {
    if (++depth > ORJSON_MAX_DEPTH) {
      return fail("Recursion limit reached");
    }
    out.push_back(bracket);
    return 0;
  }

  void close(char bracket) {
    depth--;
    if (opts & OPT_INDENT_2) {
      newline();
    }
    out.push_back(bracket);
  }

  void separate(bool first) {
    if (!first) {
      out.push_back(',');
    }
    if (opts & OPT_INDENT_2) {
      newline();
    }
  }

  int encode_list(PyObject *o) {
    bool is_list = PyList_Check(o);
    Py_ssize_t n = is_list ? PyList_GET_SIZE(o) : PyTuple_GET_SIZE(o);
    if (depth >= ORJSON_MAX_DEPTH) { // empty containers count too
      return fail("Recursion limit reached");
    }
    if (n == 0) {
      out.append("[]", 2);
      return 0;
    }
    if (open('[') < 0) {
      return -1;
    }
    for (Py_ssize_t i = 0;; i++) {
      PyObject *item = is_list ? list_item(o, i)
                       : i < n ? Py_NewRef(PyTuple_GET_ITEM(o, i))
                               : nullptr;
      if (item == nullptr) {
        break;
      }
      separate(i == 0);
      int rc = encode(item);
      Py_DECREF(item);
      if (rc < 0) {
        return -1;
      }
    }
    close(']');
    return 0;
  }

  int encode_pair(PyObject *key, PyObject *value, bool first) {
    separate(first);
    if (encode_key(key) < 0) {
      return -1;
    }
    if (opts & OPT_INDENT_2) {
      out.append(": ", 2);
    } else {
      out.push_back(':');
    }
    return encode(value);
  }

  // Sorting keys: compare the keys as they will be written (UTF-8 of str
  // keys, the text of the others).
  int encode_sorted(PyObject *o) {
    PyObject *items = PyDict_CheckExact(o) ? PyDict_Items(o) : PyMapping_Items(o);
    if (items == nullptr) {
      return -1;
    }
    struct Entry {
      std::string key;
      PyObject *pair;
    };
    std::vector<Entry> entries;
    entries.reserve(size_t(PyList_GET_SIZE(items)));
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(items); i++) {
      PyObject *pair = PyList_GET_ITEM(items, i);
      PyObject *k = PyTuple_GET_ITEM(pair, 0);
      std::string text;
      if (PyUnicode_Check(k)) {
        Py_ssize_t n;
        const char *u = PyUnicode_AsUTF8AndSize(k, &n);
        if (u == nullptr) {
          PyErr_Clear();
          Py_DECREF(items);
          return fail("str is not valid UTF-8: surrogates not allowed");
        }
        text.assign(u, size_t(n));
      } else {
        // The text of a non-str key, as written (quotes removed).
        OrjsonEncoder sub;
        sub.opts = opts;
        if (sub.encode_key(k) < 0) {
          Py_DECREF(items);
          return -1;
        }
        text.assign(sub.out.data() + 1, sub.out.size() - 2);
      }
      entries.push_back({std::move(text), pair});
    }
    std::stable_sort(entries.begin(), entries.end(),
                     [](const Entry &a, const Entry &b) { return a.key < b.key; });
    int rc = 0;
    for (size_t i = 0; rc == 0 && i < entries.size(); i++) {
      rc = encode_pair(PyTuple_GET_ITEM(entries[i].pair, 0),
                       PyTuple_GET_ITEM(entries[i].pair, 1), i == 0);
    }
    Py_DECREF(items);
    return rc;
  }

  int encode_dict(PyObject *o) {
    if (depth >= ORJSON_MAX_DEPTH) { // empty containers count too
      return fail("Recursion limit reached");
    }
    if (PyDict_GET_SIZE(o) == 0) {
      out.append("{}", 2);
      return 0;
    }
    if (open('{') < 0) {
      return -1;
    }
    int rc = 0;
#ifdef Py_GIL_DISABLED
    bool snapshot = true; // another thread may change the dict
#else
    bool snapshot = default_fn != nullptr || !PyDict_CheckExact(o); // Python code may run
#endif
    if (opts & OPT_SORT_KEYS) {
      rc = encode_sorted(o);
    } else if (snapshot) {
      PyObject *items = PyDict_CheckExact(o) ? PyDict_Items(o) : PyMapping_Items(o);
      if (items == nullptr) {
        return -1;
      }
      for (Py_ssize_t i = 0; rc == 0 && i < PyList_GET_SIZE(items); i++) {
        PyObject *pair = PyList_GET_ITEM(items, i);
        rc = encode_pair(PyTuple_GET_ITEM(pair, 0), PyTuple_GET_ITEM(pair, 1), i == 0);
      }
      Py_DECREF(items);
    } else {
      Py_ssize_t pos = 0;
      PyObject *key, *value;
      bool first = true;
      while (rc == 0 && PyDict_Next(o, &pos, &key, &value)) {
        rc = encode_pair(key, value, first);
        first = false;
      }
    }
    if (rc == 0) {
      close('}');
    }
    return rc;
  }

  int encode_default(PyObject *o) {
    if (default_fn == nullptr) {
      PyErr_Format(PyExc_TypeError, "Type is not JSON serializable: %s", Py_TYPE(o)->tp_name);
      return -1;
    }
    if (++default_calls > ORJSON_MAX_DEPTH) {
      return fail("default serializer exceeds recursion limit");
    }
    PyObject *r = PyObject_CallOneArg(default_fn, o);
    if (r == nullptr) {
      // TypeError("Type is not JSON serializable: ...") from the exception
      // that default raised, as orjson does.
      PyObject *typ, *cause, *tb;
      PyErr_Fetch(&typ, &cause, &tb);
      PyErr_NormalizeException(&typ, &cause, &tb);
      if (tb != nullptr) {
        PyException_SetTraceback(cause, tb);
      }
      Py_XDECREF(typ);
      Py_XDECREF(tb);
      PyErr_Format(PyExc_TypeError, "Type is not JSON serializable: %s", Py_TYPE(o)->tp_name);
      PyObject *t2, *exc, *tb2;
      PyErr_Fetch(&t2, &exc, &tb2);
      PyErr_NormalizeException(&t2, &exc, &tb2);
      PyException_SetContext(exc, Py_NewRef(cause));
      PyException_SetCause(exc, cause); // steals cause
      PyErr_Restore(t2, exc, tb2);
      return -1;
    }
    int rc = encode(r);
    Py_DECREF(r);
    default_calls--;
    return rc;
  }

  int encode(PyObject *o) {
    PyTypeObject *t = Py_TYPE(o);
    if (t == &PyUnicode_Type) {
      return encode_str(o);
    }
    if (t == &PyLong_Type) {
      return encode_int(o);
    }
    if (t == &PyFloat_Type) {
      encode_float(PyFloat_AS_DOUBLE(o));
      return 0;
    }
    if (t == &PyDict_Type) {
      return encode_dict(o);
    }
    if (t == &PyList_Type || t == &PyTuple_Type) {
      return encode_list(o);
    }
    if (o == Py_None || o == Py_True || o == Py_False) {
      if (o == Py_None) {
        out.append("null", 4);
      } else if (o == Py_True) {
        out.append("true", 4);
      } else {
        out.append("false", 5);
      }
      return 0;
    }
    if (PyObject_IsInstance(o, EnumType) == 1) {
      PyObject *value = PyObject_GetAttrString(o, "value");
      if (value == nullptr) {
        return -1;
      }
      int rc = encode(value);
      Py_DECREF(value);
      return rc;
    }
    if (!(opts & OPT_PASSTHROUGH_SUBCLASS)) {
      if (PyUnicode_Check(o)) {
        return encode_str(o);
      }
      if (PyLong_Check(o)) {
        return encode_int(o);
      }
      if (PyDict_Check(o)) {
        return encode_dict(o);
      }
      if (PyList_Check(o)) {
        return encode_list(o);
      }
    }
    if (PyFloat_Check(o)) {
      encode_float(PyFloat_AS_DOUBLE(o));
      return 0;
    }
    if (PyTuple_Check(o)) {
      return encode_list(o);
    }
    return encode_default(o);
  }
};

// dumpb(obj, /, default=None, option=None), as orjson.dumps.
PyObject *dumpb(PyObject *, PyObject *const *args, Py_ssize_t nargs, PyObject *kwnames) {
  PyObject *obj = nullptr, *dflt = nullptr, *option = nullptr;
  Py_ssize_t nkw = kwnames ? PyTuple_GET_SIZE(kwnames) : 0;
  if (nargs < 1 || nargs > 3) {
    PyErr_SetString(PyExc_TypeError, "dumpb() takes 1 to 3 positional arguments");
    return nullptr;
  }
  obj = args[0];
  dflt = nargs > 1 ? args[1] : nullptr;
  option = nargs > 2 ? args[2] : nullptr;
  for (Py_ssize_t i = 0; i < nkw; i++) {
    PyObject *name = PyTuple_GET_ITEM(kwnames, i);
    if (PyUnicode_CompareWithASCIIString(name, "default") == 0 && dflt == nullptr) {
      dflt = args[nargs + i];
    } else if (PyUnicode_CompareWithASCIIString(name, "option") == 0 && option == nullptr) {
      option = args[nargs + i];
    } else {
      PyErr_Format(PyExc_TypeError, "dumpb() got an unexpected keyword argument '%U'", name);
      return nullptr;
    }
  }
  OrjsonEncoder e;
  if (dflt != nullptr && dflt != Py_None) {
    if (!PyCallable_Check(dflt)) {
      PyErr_SetString(PyExc_TypeError, "default must be callable");
      return nullptr;
    }
    e.default_fn = dflt;
  }
  if (option != nullptr && option != Py_None) {
    if (!PyLong_Check(option)) {
      PyErr_SetString(PyExc_TypeError, "Invalid opts");
      return nullptr;
    }
    long v = PyLong_AsLong(option);
    if ((v == -1 && PyErr_Occurred()) || v < 0 || (unsigned long)v > OPT_ALL) {
      PyErr_Clear();
      PyErr_SetString(PyExc_TypeError, "Invalid opts");
      return nullptr;
    }
    e.opts = (unsigned long)v;
  }
  int rc;
  try {
    rc = e.encode(obj);
    if (rc == 0 && (e.opts & OPT_APPEND_NEWLINE)) {
      e.out.push_back('\n');
    }
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  }
  if (rc < 0) {
    return nullptr;
  }
  return PyBytes_FromStringAndSize(e.out.data(), Py_ssize_t(e.out.size()));
}

// dump(obj, fp, **kw): fp.write(dumps(obj, **kw)).
PyObject *dump(PyObject *, PyObject *args, PyObject *kwargs) {
  if (PyTuple_GET_SIZE(args) != 2) {
    PyErr_SetString(PyExc_TypeError, "dump() takes exactly 2 positional arguments (obj, fp)");
    return nullptr;
  }
  PyObject *obj_only = PyTuple_GetSlice(args, 0, 1);
  if (obj_only == nullptr) {
    return nullptr;
  }
  PyObject *s = dumps(nullptr, obj_only, kwargs);
  Py_DECREF(obj_only);
  if (s == nullptr) {
    return nullptr;
  }
  PyObject *r = PyObject_CallMethod(PyTuple_GET_ITEM(args, 1), "write", "O", s);
  Py_DECREF(s);
  if (r == nullptr) {
    return nullptr;
  }
  Py_DECREF(r);
  Py_RETURN_NONE;
}

// ---------------------------------------------------------------------------
// Files: load(fp), load_file(path), parse_file(path).
// ---------------------------------------------------------------------------
PyObject *io_open = nullptr;

// The bytes of the file at path.
PyObject *read_file(PyObject *path) {
  PyObject *f = PyObject_CallFunction(io_open, "Os", path, "rb");
  if (f == nullptr) {
    return nullptr;
  }
  PyObject *data = PyObject_CallMethod(f, "read", nullptr);
  PyObject *r = PyObject_CallMethod(f, "close", nullptr);
  Py_DECREF(f);
  if (r == nullptr) {
    Py_XDECREF(data);
    return nullptr;
  }
  Py_DECREF(r);
  return data;
}

// Parses data, a new reference (or nullptr after an error), and releases it.
template <PyObject *(*Parse)(const char *, size_t, PyObject *)>
PyObject *parse_and_release(PyObject *data) {
  if (data == nullptr) {
    return nullptr;
  }
  PyObject *r = with_input<Parse>(data);
  Py_DECREF(data);
  return r;
}

PyObject *load(PyObject *, PyObject *fp) {
  return parse_and_release<parse_buffer>(PyObject_CallMethod(fp, "read", nullptr));
}

PyObject *load_file(PyObject *, PyObject *path) {
  return parse_and_release<parse_buffer>(read_file(path));
}

PyObject *parse_file(PyObject *, PyObject *path) {
  return parse_and_release<parse_lazy_buffer>(read_file(path));
}

// ---------------------------------------------------------------------------
// Streams: loads_many(data) and parse_many(data) iterate over the documents
// of a buffer: whitespace-separated documents (including NDJSON / JSON
// Lines), one document per line, RFC 7464 sequences, comma-separated
// documents, or the elements of one array.
// ---------------------------------------------------------------------------
using simdjson::dom::document_stream;

PyTypeObject *StreamType = nullptr;
PyObject *json_raw_decode = nullptr; // json._default_decoder.raw_decode

struct StreamObject {
  PyObject_HEAD
  PyObject *source;               // the input object
  simdjson::padded_string *copy;  // padded copy of the input, if needed
  const char *buf;
  size_t len;
  size_t batch_size;
  simdjson::stream_format format;
  bool lazy;
  bool done;
  bool active;     // a simdjson stream is running from `base`
  size_t base;     // offset of the running stream in buf
  size_t last_end; // offset just past the last document returned
  size_t prefix;   // bytes that parse_many skips at the start (see make_stream)
  size_t open;     // array format: offsets of the outer '[' and ']'
  size_t close;
  PyObject *text;  // the input as str, built on the error path
  parser *p;
  document_stream *stream;
  document_stream::iterator it;
};

inline bool json_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Offset of the first byte at or after `at` that is neither white space nor
// a separator of the stream format, nor the prefix that simdjson skips (a
// byte order mark and, for the array format, the opening '['). For the array
// format, the closing ']' is a separator too, but no other bracket.
size_t skip_separators(const StreamObject *s, size_t at) {
  using simdjson::stream_format;
  if (at < s->prefix) {
    at = s->prefix;
  }
  for (; at < s->len; at++) {
    char c = s->buf[at];
    bool sep = json_space(c) ||
               (s->format == stream_format::json_sequence && c == '\x1e') ||
               ((s->format == stream_format::comma_delimited ||
                 s->format == stream_format::comma_delimited_array) && c == ',') ||
               at == s->close;
    if (!sep) {
      break;
    }
  }
  return at;
}

// Number of code points in the UTF-8 bytes [0, n).
Py_ssize_t char_index(const char *s, size_t n) {
  Py_ssize_t count = 0;
  for (size_t i = 0; i < n; i++) {
    count += (uint8_t(s[i]) & 0xC0) != 0x80;
  }
  return count;
}

// Byte offset of the code point that follows `chars` code points from
// byte offset `from`.
size_t byte_offset(const char *s, size_t len, size_t from, Py_ssize_t chars) {
  size_t i = from;
  while (chars > 0 && i < len) {
    i++;
    while (i < len && (uint8_t(s[i]) & 0xC0) == 0x80) {
      i++;
    }
    chars--;
  }
  return i;
}

PyObject *stream_text(StreamObject *s) {
  if (s->text == nullptr) {
    s->text = PyUnicode_Check(s->source)
                  ? Py_NewRef(s->source)
                  : PyUnicode_DecodeUTF8(s->buf, Py_ssize_t(s->len), nullptr);
  }
  return s->text;
}

// Raises JSONDecodeError at byte offset pos of the input.
void stream_error(StreamObject *s, const char *msg, size_t pos) {
  PyObject *text = stream_text(s);
  if (text == nullptr) {
    return;
  }
  PyObject *exc = PyObject_CallFunction(JSONDecodeError, "sOn", msg, text,
                                        char_index(s->buf, pos));
  if (exc != nullptr) {
    PyErr_SetObject(JSONDecodeError, exc);
    Py_DECREF(exc);
  }
}

bool stream_start(StreamObject *s, size_t at) {
  delete s->stream;
  s->stream = new (std::nothrow) document_stream();
  if (s->stream == nullptr) {
    PyErr_NoMemory();
    return false;
  }
  // The first start parses the whole input in its format. A restart (after a
  // document that json decoded, or with larger batches) begins after a
  // document: comma-separated documents restart at the next one, and the
  // rest of an array is parsed as comma-separated documents before its ']'.
  using simdjson::stream_format;
  stream_format fmt = s->format;
  size_t end = s->len;
  if (at > 0) {
    if (fmt == stream_format::comma_delimited_array) {
      fmt = stream_format::comma_delimited;
      end = s->close < s->len ? s->close : s->len;
    }
    if (fmt == stream_format::comma_delimited) {
      at = skip_separators(s, at);
    }
  }
  auto r = s->p->parse_many(reinterpret_cast<const uint8_t *>(s->buf) + at,
                            at < end ? end - at : 0, s->batch_size, fmt);
  simdjson::error_code err = std::move(r).get(*s->stream);
  if (err) {
    stream_error(s, simdjson::error_message(err), at);
    return false;
  }
  s->it = s->stream->begin();
  // At 0, offsets are relative to what follows the prefix.
  s->base = at == 0 ? s->prefix : at;
  s->active = true;
  return true;
}

// A copy of the parser's current document, owning its own buffers.
document *copy_document(const document &src) {
  const uint64_t *tape = src.tape.get();
  // The root entry holds the index just past the last tape entry.
  size_t n = size_t(tape[0] & simdjson::internal::JSON_VALUE_MASK);
  size_t strings = 0;
  for (size_t i = 0; i < n; i++) {
    uint8_t type = uint8_t(tape[i] >> 56);
    if (type == '"' || type == 'Z') {
      size_t idx = size_t(tape[i] & simdjson::internal::JSON_VALUE_MASK);
      uint32_t l;
      memcpy(&l, src.string_buf.get() + idx, sizeof(l));
      strings = std::max(strings, idx + sizeof(uint32_t) + l + 1);
    } else if (type == 'l' || type == 'u' || type == 'd') {
      i++;
    }
  }
  document *d = new (std::nothrow) document();
  if (d == nullptr) {
    return nullptr;
  }
  d->tape.reset(new (std::nothrow) uint64_t[n]);
  d->string_buf.reset(new (std::nothrow) uint8_t[strings + simdjson::SIMDJSON_PADDING]());
  if (!d->tape || !d->string_buf) {
    delete d;
    return nullptr;
  }
  memcpy(d->tape.get(), tape, n * sizeof(uint64_t));
  memcpy(d->string_buf.get(), src.string_buf.get(), strings);
  return d;
}

PyObject *stream_value(StreamObject *s) {
  if (!s->lazy) {
    return build_with_gc_paused(s->p->doc, 1);
  }
  document *d = copy_document(s->p->doc);
  return d == nullptr ? PyErr_NoMemory() : view_of(d);
}

// simdjson rejected the document after last_end. json decides, as in loads:
// return the value it accepts and resume after it, or raise its error.
PyObject *stream_fallback(StreamObject *s) {
  size_t at = skip_separators(s, s->last_end);
  PyObject *text = stream_text(s);
  if (text == nullptr) {
    return nullptr;
  }
  Py_ssize_t start = char_index(s->buf, at);
  PyObject *r = PyObject_CallFunction(json_raw_decode, "On", text, start);
  if (r == nullptr) {
    reraise_decode_error(); // as in loads
    return nullptr;
  }
  PyObject *value = Py_NewRef(PyTuple_GET_ITEM(r, 0));
  Py_ssize_t end = PyLong_AsSsize_t(PyTuple_GET_ITEM(r, 1));
  Py_DECREF(r);
  if (end < 0) {
    Py_DECREF(value);
    return nullptr;
  }
  s->last_end = byte_offset(s->buf, s->len, at, end - start);
  // Between documents, the formats other than white space need their
  // separator: json decoded one document, not the separator after it.
  using simdjson::stream_format;
  size_t next = s->last_end;
  while (next < s->len && json_space(s->buf[next])) {
    next++;
  }
  char sep = s->format == stream_format::json_sequence ? '\x1e'
             : s->format == stream_format::comma_delimited ||
                     s->format == stream_format::comma_delimited_array
                 ? ','
                 : 0;
  if (sep != 0 && next < s->len && next != s->close && s->buf[next] != sep) {
    Py_DECREF(value);
    stream_error(s, sep == ',' ? "Expecting ',' delimiter" : "Expecting record separator", next);
    return nullptr;
  }
  s->active = false; // resume with simdjson after this document
  // As with parse(), a document that only json accepts is returned as plain
  // Python objects.
  return value;
}

// The next document, or nullptr at the end or after an error.
PyObject *stream_next_locked(StreamObject *s) {
  for (;;) {
    if (!s->active) {
      if (skip_separators(s, s->last_end) >= s->len) {
        return nullptr;
      }
      if (!stream_start(s, s->last_end)) {
        return nullptr;
      }
    }
    if (!(s->it != s->stream->end())) {
      // Anything but separators after the last document is an incomplete
      // document: let json report it.
      return skip_separators(s, s->last_end) < s->len ? stream_fallback(s) : nullptr;
    }
    simdjson::dom::element el;
    simdjson::error_code err = (*s->it).get(el);
    if (!err) {
      size_t end = s->base + s->it.current_index() + s->it.source().size();
      PyObject *v = stream_value(s);
      if (v != nullptr) {
        s->last_end = end;
        ++s->it;
      }
      return v;
    }
    if (err == simdjson::CAPACITY && s->batch_size < s->len) {
      // A document larger than the batch: restart there with larger batches.
      s->batch_size = std::min(s->len, s->batch_size * 4);
      s->active = false;
      continue;
    }
    return stream_fallback(s);
  }
}

PyObject *stream_next(PyObject *self) {
  StreamObject *s = reinterpret_cast<StreamObject *>(self);
  PyObject *r = nullptr;
  Py_BEGIN_CRITICAL_SECTION(self);
  if (!s->done) {
    r = stream_next_locked(s);
    s->done = r == nullptr; // the end, or an error: the stream is over
  }
  Py_END_CRITICAL_SECTION();
  return r;
}

void stream_dealloc(PyObject *self) {
  StreamObject *s = reinterpret_cast<StreamObject *>(self);
  PyTypeObject *tp = Py_TYPE(self);
  s->it.~iterator();
  delete s->stream;
  delete s->p;
  delete s->copy;
  Py_XDECREF(s->source);
  Py_XDECREF(s->text);
  PyObject_Free(self);
  Py_DECREF(tp);
}

PyType_Slot stream_slots[] = {
    {Py_tp_dealloc, reinterpret_cast<void *>(stream_dealloc)},
    {Py_tp_iter, reinterpret_cast<void *>(PyObject_SelfIter)},
    {Py_tp_iternext, reinterpret_cast<void *>(stream_next)},
    {0, nullptr},
};

PyType_Spec stream_spec = {
    "fastsimdjson._Stream", sizeof(StreamObject), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION, stream_slots,
};

PyObject *make_stream(PyObject *args, PyObject *kwargs, bool lazy) {
  static const char *kwlist[] = {"data", "format", "batch_size", nullptr};
  PyObject *data;
  const char *format = "whitespace";
  Py_ssize_t batch_size = Py_ssize_t(simdjson::dom::DEFAULT_BATCH_SIZE);
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|$sn",
                                   const_cast<char **>(kwlist), &data, &format,
                                   &batch_size)) {
    return nullptr;
  }
  simdjson::stream_format fmt;
  if (strcmp(format, "whitespace") == 0) {
    fmt = simdjson::stream_format::whitespace_delimited;
  } else if (strcmp(format, "lines") == 0) {
    fmt = simdjson::stream_format::newline_delimited;
  } else if (strcmp(format, "json_seq") == 0) {
    fmt = simdjson::stream_format::json_sequence;
  } else if (strcmp(format, "comma") == 0) {
    fmt = simdjson::stream_format::comma_delimited;
  } else if (strcmp(format, "array") == 0) {
    fmt = simdjson::stream_format::comma_delimited_array;
  } else {
    PyErr_Format(PyExc_ValueError,
                 "format must be 'whitespace', 'lines', 'json_seq', 'comma' or "
                 "'array', not '%s'", format);
    return nullptr;
  }
  if (batch_size < 64) {
    PyErr_SetString(PyExc_ValueError, "batch_size must be at least 64");
    return nullptr;
  }
  const char *buf = nullptr;
  Py_ssize_t len = 0;
  bool must_copy = false;
  if (PyBytes_Check(data)) {
    buf = PyBytes_AS_STRING(data);
    len = PyBytes_GET_SIZE(data);
  } else if (PyUnicode_Check(data)) {
    buf = PyUnicode_AsUTF8AndSize(data, &len);
    if (buf == nullptr) {
      return nullptr;
    }
  } else if (PyByteArray_Check(data) || PyMemoryView_Check(data)) {
    must_copy = true; // mutable: parse a copy
  } else {
    PyErr_Format(PyExc_TypeError,
                 "Input must be bytes, bytearray, memoryview, or str, not %.200s",
                 Py_TYPE(data)->tp_name);
    return nullptr;
  }
  StreamObject *s = PyObject_New(StreamObject, StreamType);
  if (s == nullptr) {
    return nullptr;
  }
  s->source = nullptr;
  s->copy = nullptr;
  s->text = nullptr;
  s->stream = nullptr;
  s->p = nullptr;
  new (&s->it) document_stream::iterator();
  s->format = fmt;
  s->batch_size = size_t(batch_size);
  s->lazy = lazy;
  s->done = false;
  s->active = false;
  s->base = 0;
  s->last_end = 0;
  PyObject *self = reinterpret_cast<PyObject *>(s);
  if (must_copy) {
    PyObject *b = PyBytes_FromObject(data);
    if (b == nullptr) {
      Py_DECREF(self);
      return nullptr;
    }
    s->source = b;
    buf = PyBytes_AS_STRING(b);
    len = PyBytes_GET_SIZE(b);
  } else {
    s->source = Py_NewRef(data);
  }
  if (needs_unpadded(buf, size_t(len))) {
    s->copy = new (std::nothrow) simdjson::padded_string(buf, size_t(len));
    if (s->copy == nullptr) {
      Py_DECREF(self);
      return PyErr_NoMemory();
    }
    buf = s->copy->data();
  }
  s->buf = buf;
  s->len = size_t(len);
  // parse_many skips a UTF-8 byte order mark and, for the array format,
  // white space and the opening '['; the offsets it reports start after.
  size_t i = s->len >= 3 && memcmp(buf, "\xEF\xBB\xBF", 3) == 0 ? 3 : 0;
  s->open = s->close = size_t(-1);
  if (fmt == simdjson::stream_format::comma_delimited_array) {
    while (i < s->len && json_space(buf[i])) {
      i++;
    }
    size_t j = s->len;
    while (j > i && json_space(buf[j - 1])) {
      j--;
    }
    if (i < s->len && buf[i] == '[') {
      s->open = i++;
    }
    if (j > i && buf[j - 1] == ']') {
      s->close = j - 1;
    }
  }
  s->prefix = i;
  if (fmt != simdjson::stream_format::whitespace_delimited &&
      fmt != simdjson::stream_format::newline_delimited && s->batch_size < s->len) {
    // A restart in the middle of the input would miss a separator, so one
    // batch covers the whole input.
    s->batch_size = s->len;
  }
  s->p = new (std::nothrow) parser();
  if (s->p == nullptr) {
    Py_DECREF(self);
    return PyErr_NoMemory();
  }
  s->p->number_as_string(true);
  return self;
}

PyObject *loads_many(PyObject *, PyObject *args, PyObject *kwargs) {
  return make_stream(args, kwargs, false);
}

PyObject *parse_many(PyObject *, PyObject *args, PyObject *kwargs) {
  return make_stream(args, kwargs, true);
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
  auto err = parse_into(g_thread_parser.ptr->doc, buf, len);
  release_large_parser();
  return PyLong_FromLong(long(err));
}

PyObject *release(PyObject *, PyObject *) {
  release_parser();
  delete g_thread_parser.spare;
  g_thread_parser.spare = nullptr;
  clear_cache(g_thread_parser.key_cache);
  clear_cache(g_thread_parser.value_cache);
  g_thread_parser.clear_encoded_keys();
  Py_RETURN_NONE;
}

PyMethodDef methods[] = {
    {"_parse_only", parse_only, METH_O, "Parse without building objects."},
    {"loads", loads, METH_O, "Deserialize JSON to Python objects."},
    {"dumps", reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(dumps)),
     METH_VARARGS | METH_KEYWORDS,
     "Serialize obj to a JSON str. Same arguments and output as json.dumps."},
    {"dumpb", reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(dumpb)),
     METH_FASTCALL | METH_KEYWORDS,
     "Serialize obj to JSON bytes, with the arguments and output of orjson.dumps."},
    {"dump", reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(dump)),
     METH_VARARGS | METH_KEYWORDS,
     "Serialize obj as JSON to fp (a file with a write method), like json.dump."},
    {"load", load, METH_O, "Deserialize JSON from fp (a file with a read method), like json.load."},
    {"load_file", load_file, METH_O, "Deserialize the JSON file at path, like loads."},
    {"parse_file", parse_file, METH_O, "Parse the JSON file at path lazily, like parse."},
    {"loads_many", reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(loads_many)),
     METH_VARARGS | METH_KEYWORDS,
     "Iterate over the JSON documents of data (NDJSON, JSON Lines, ...), like loads."},
    {"parse_many", reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(parse_many)),
     METH_VARARGS | METH_KEYWORDS,
     "Iterate over the JSON documents of data lazily, like parse."},
    {"parse", parse, METH_O,
     "Parse JSON lazily: objects and arrays are returned as read-only views "
     "(Object, Array) whose values are converted on access."},
    {"release", release, METH_NOARGS,
     "Release the simdjson parser and string caches retained by this thread."},
    {nullptr, nullptr, 0, nullptr},
};

// collections.abc.<name>.register(type)
int register_abc(const char *name, PyTypeObject *type) {
  PyObject *abc = PyImport_ImportModule("collections.abc");
  PyObject *cls = abc ? PyObject_GetAttrString(abc, name) : nullptr;
  PyObject *r = cls ? PyObject_CallMethod(cls, "register", "O", type) : nullptr;
  Py_XDECREF(abc);
  Py_XDECREF(cls);
  Py_XDECREF(r);
  return r == nullptr ? -1 : 0;
}

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
    json_dumps = PyObject_GetAttrString(json, "dumps");
    PyObject *decoder = PyObject_GetAttrString(json, "_default_decoder");
    if (decoder != nullptr) {
      json_raw_decode = PyObject_GetAttrString(decoder, "raw_decode");
      Py_DECREF(decoder);
    }
    PyObject *io = PyImport_ImportModule("io");
    if (io != nullptr) {
      io_open = PyObject_GetAttrString(io, "open");
      Py_DECREF(io);
    }
    Py_DECREF(json);
    if (json_JSONDecodeError == nullptr || json_loads == nullptr ||
        json_dumps == nullptr || json_raw_decode == nullptr || io_open == nullptr) {
      Py_CLEAR(json_JSONDecodeError);
      Py_CLEAR(json_loads);
      Py_CLEAR(json_dumps);
      Py_CLEAR(json_raw_decode);
      Py_CLEAR(io_open);
      return -1;
    }
  }
  if (EnumType == nullptr) {
    PyObject *enum_module = PyImport_ImportModule("enum");
    EnumType = enum_module ? PyObject_GetAttrString(enum_module, "Enum") : nullptr;
    Py_XDECREF(enum_module);
    if (EnumType == nullptr) {
      return -1;
    }
  }
  struct {
    const char *name;
    long value;
  } options[] = {{"OPT_APPEND_NEWLINE", 1024}, {"OPT_INDENT_2", 1},
                 {"OPT_NAIVE_UTC", 2},          {"OPT_NON_STR_KEYS", 4},
                 {"OPT_OMIT_MICROSECONDS", 8},  {"OPT_PASSTHROUGH_DATACLASS", 2048},
                 {"OPT_PASSTHROUGH_DATETIME", 512}, {"OPT_PASSTHROUGH_SUBCLASS", 256},
                 {"OPT_SERIALIZE_DATACLASS", 0}, {"OPT_SERIALIZE_NUMPY", 16},
                 {"OPT_SERIALIZE_UUID", 0},     {"OPT_SORT_KEYS", 32},
                 {"OPT_STRICT_INTEGER", 64},    {"OPT_UTC_Z", 128}};
  for (auto &o : options) {
    if (PyModule_AddIntConstant(module, o.name, o.value) < 0) {
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
  if (PyModule_AddObjectRef(module, "JSONDecodeError", JSONDecodeError) < 0) {
    return -1;
  }
  struct {
    PyTypeObject **type;
    PyType_Spec *spec;
  } types[] = {{&DocumentType, &document_spec}, {&ObjectType, &object_spec},
               {&ArrayType, &array_spec},       {&IterType, &iter_spec},
               {&StreamType, &stream_spec}};
  for (auto &t : types) {
    if (*t.type == nullptr) {
      *t.type = reinterpret_cast<PyTypeObject *>(PyType_FromSpec(t.spec));
      if (*t.type == nullptr) {
        return -1;
      }
    }
  }
  // isinstance(x, Mapping) / isinstance(x, Sequence) hold for the views.
  if (register_abc("Mapping", ObjectType) < 0 || register_abc("Sequence", ArrayType) < 0 ||
      PyModule_AddObjectRef(module, "Object", reinterpret_cast<PyObject *>(ObjectType)) < 0 ||
      PyModule_AddObjectRef(module, "Array", reinterpret_cast<PyObject *>(ArrayType)) < 0) {
    return -1;
  }
  return 0;
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
