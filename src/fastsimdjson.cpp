// Fast Python binding for simdjson: parse JSON into native Python objects.
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
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
// and release() can delete it. `spare` is a document released by parse()
// (see the lazy API below), kept for the next parse() on this thread.
struct ThreadParser {
  parser *ptr = nullptr;
  simdjson::dom::document *spare = nullptr;
  KeyCacheEntry key_cache[KEY_CACHE_SIZE]{};
  KeyCacheEntry value_cache[KEY_CACHE_SIZE]{};
  ~ThreadParser() {
    delete ptr;
    ptr = nullptr;
    delete spare;
    spare = nullptr;
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
#if defined(__SANITIZE_ADDRESS__)
  // AddressSanitizer reports the in-page read past the end of the buffer.
  return true;
#endif
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

// Suspend the cyclic GC while building: the new containers cannot form
// cycles, and collections triggered by the many allocations are wasted work.
// PyGC_Disable is process-global, so free-threaded builds leave GC alone.
PyObject *build_with_gc_paused(Builder &b, size_t i) {
#ifndef Py_GIL_DISABLED
  int gc_was_enabled = PyGC_Disable();
#endif
  PyObject *res = b.build(i);
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
  simdjson::error_code err = parse_input(buf, len);
  if (err) {
    // Drop a huge parser before the json.loads rescan allocates again.
    release_large_parser();
    return fallback_loads(original);
  }
  Builder b{g_thread_parser.ptr->doc.tape.get(),
            g_thread_parser.ptr->doc.string_buf.get(),
            g_thread_parser.key_cache, g_thread_parser.value_cache};
  PyObject *res = build_with_gc_paused(b, 1); // skip the root entry
  release_large_parser();
  return res;
}

// Calls Parse on the bytes of arg (bytes, str, bytearray or memoryview).
template <PyObject *(*Parse)(const char *, size_t, PyObject *)>
PyObject *with_input(PyObject *arg) {
  if (PyBytes_Check(arg)) {
    return Parse(PyBytes_AS_STRING(arg), size_t(PyBytes_GET_SIZE(arg)),
                        arg);
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
    PyObject *res = Parse(PyBytes_AS_STRING(copy),
                                 size_t(PyBytes_GET_SIZE(copy)), arg);
    Py_DECREF(copy);
    return res;
#else
    if (PyByteArray_Check(arg)) {
      return Parse(PyByteArray_AS_STRING(arg),
                          size_t(PyByteArray_GET_SIZE(arg)), arg);
    }
    Py_buffer *view = PyMemoryView_GET_BUFFER(arg);
    return Parse(static_cast<const char *>(view->buf),
                        size_t(view->len), arg);
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

using simdjson::dom::document;

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

inline Builder builder_of(DocumentObject *owner) {
  return Builder{owner->doc->tape.get(), owner->doc->string_buf.get(),
                 g_thread_parser.key_cache, g_thread_parser.value_cache};
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
  default: {
    Builder b = builder_of(owner);
    return b.build(i);
  }
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

// Tape index of element k of the array at start, or 0.
size_t find_index(ProxyObject *a, size_t k) {
  const uint64_t *tape = tape_of(a->owner);
  size_t end = close_of(tape, a->start);
  size_t i = a->start + 1;
  size_t j = 0;
  uint64_t c = a->cursor.load(std::memory_order_relaxed);
  if (c != 0 && size_t(c >> 32) <= k) {
    j = size_t(c >> 32);
    i = size_t(c & 0xFFFFFFFF);
  }
  for (; j < k && i < end; j++) {
    i = skip(tape, i);
  }
  if (i >= end) {
    return 0;
  }
  a->cursor.store((uint64_t(k) << 32) | i, std::memory_order_relaxed);
  return i;
}

PyObject *materialize(DocumentObject *owner, size_t i) {
  Builder b = builder_of(owner);
  return build_with_gc_paused(b, i);
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
      size_t close = close_of(tape, i);
      size_t e = i + 1;
      for (size_t j = 0; j < k && e < close; j++) {
        e = skip(tape, e);
      }
      if (e >= close) {
        PyErr_Format(PyExc_IndexError, "array index out of range in JSON pointer: %R", arg);
        return nullptr;
      }
      i = e;
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
  return materialize(proxy(self)->owner, proxy(self)->start);
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
  Builder b = builder_of(it->owner);
  const char *s = b.str_at(tape[pos], &len);
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
  const uint8_t *u = reinterpret_cast<const uint8_t *>(buf);
  simdjson::error_code err =
      needs_unpadded(buf, len)
          ? g_thread_parser.ptr->parse_into_document_unpadded(*d, u, len).error()
          : g_thread_parser.ptr->parse_into_document(*d, u, len, false).error();
  release_large_parser();
  if (err) {
    give_back(d);
    return fallback_loads(original);
  }
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

PyObject *parse(PyObject *, PyObject *arg) {
  return with_input<parse_lazy_buffer>(arg);
}

// Creates the lazy types and adds them to the module.
int add_lazy_types(PyObject *module) {
  if (ObjectType == nullptr) {
    DocumentType = reinterpret_cast<PyTypeObject *>(PyType_FromSpec(&document_spec));
    ObjectType = reinterpret_cast<PyTypeObject *>(PyType_FromSpec(&object_spec));
    ArrayType = reinterpret_cast<PyTypeObject *>(PyType_FromSpec(&array_spec));
    IterType = reinterpret_cast<PyTypeObject *>(PyType_FromSpec(&iter_spec));
    if (DocumentType == nullptr || ObjectType == nullptr || ArrayType == nullptr ||
        IterType == nullptr) {
      Py_CLEAR(DocumentType);
      Py_CLEAR(ObjectType);
      Py_CLEAR(ArrayType);
      Py_CLEAR(IterType);
      return -1;
    }
    // isinstance(x, Mapping) / isinstance(x, Sequence) hold for the views.
    PyObject *abc = PyImport_ImportModule("collections.abc");
    if (abc == nullptr) {
      return -1;
    }
    PyObject *mapping = PyObject_GetAttrString(abc, "Mapping");
    PyObject *sequence = PyObject_GetAttrString(abc, "Sequence");
    Py_DECREF(abc);
    PyObject *a = mapping ? PyObject_CallMethod(mapping, "register", "O", ObjectType) : nullptr;
    PyObject *b = sequence ? PyObject_CallMethod(sequence, "register", "O", ArrayType) : nullptr;
    Py_XDECREF(mapping);
    Py_XDECREF(sequence);
    if (a == nullptr || b == nullptr) {
      Py_XDECREF(a);
      Py_XDECREF(b);
      return -1;
    }
    Py_DECREF(a);
    Py_DECREF(b);
  }
  if (PyModule_AddObjectRef(module, "Object", reinterpret_cast<PyObject *>(ObjectType)) < 0 ||
      PyModule_AddObjectRef(module, "Array", reinterpret_cast<PyObject *>(ArrayType)) < 0) {
    return -1;
  }
  return 0;
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
  delete g_thread_parser.spare;
  g_thread_parser.spare = nullptr;
  release_caches();
  Py_RETURN_NONE;
}

PyMethodDef methods[] = {
    {"_parse_only", parse_only, METH_O, "Parse without building objects."},
    {"loads", loads, METH_O, "Deserialize JSON to Python objects."},
    {"parse", parse, METH_O,
     "Parse JSON lazily: objects and arrays are returned as read-only views "
     "(Object, Array) whose values are converted on access."},
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
  if (PyModule_AddObjectRef(module, "JSONDecodeError", JSONDecodeError) < 0) {
    return -1;
  }
  return add_lazy_types(module);
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
