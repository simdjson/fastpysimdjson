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
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

// simdjson.cpp is compiled here (a unity build) so that dumps can call its
// float-to-decimal routine, dtoa_impl::dragonbox, directly.
#include "simdjson.h"
#include "simdjson.cpp"
#include "simdjson_arrow.h" // NDJSON to Arrow and back (vendor/simdjson_arrow.cpp)
#include "simdjson_msgpack.h" // JSON to MessagePack and back (vendor/simdjson_msgpack.cpp)
#include "simdutf.h"
#include "zmij.h" // shortest float digits for dumps and dumpb (in vendor/simdjson_arrow.cpp)

#ifdef _MSC_VER
#include <intrin.h>
inline int ctz64(uint64_t x) {
  unsigned long i;
  _BitScanForward64(&i, x);
  return int(i);
}
inline int clz64(uint64_t x) {
  unsigned long i;
  _BitScanReverse64(&i, x);
  return 63 - int(i);
}
#else
inline int ctz64(uint64_t x) { return __builtin_ctzll(x); }
inline int clz64(uint64_t x) { return __builtin_clzll(x); }
#endif

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <cpuid.h>
#include <immintrin.h>
#endif
#define FSJ_SSE2 1
#elif defined(__ARM_NEON) || defined(_M_ARM64)
#include <arm_neon.h>
#define FSJ_NEON 1
#endif

#if defined(_MSC_VER) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define FSJ_LITTLE_ENDIAN 1
#endif

#if PY_VERSION_HEX >= 0x030D0000
// No longer declared in the public headers since 3.13, but still exported.
extern "C" int _PyDict_SetItem_KnownHash(PyObject *mp, PyObject *key,
                                         PyObject *item, Py_hash_t hash);
#endif
#if defined(Py_GIL_DISABLED) && PY_VERSION_HEX >= 0x030E0000
extern "C" void _Py_SetImmortal(PyObject *op); // exported, not declared
#endif

#if defined(Py_GIL_DISABLED) && PY_VERSION_HEX >= 0x030D0000
// Without the GIL, _PyDict_SetItem_KnownHash locks the dict for each item.
// The dicts that loads builds are new and seen by no other thread until
// they are returned: they are filled without the lock.
extern "C" int _PyDict_SetItem_KnownHash_LockHeld(PyDictObject *mp, PyObject *key,
                                                  PyObject *item, Py_hash_t hash);
inline int new_dict_set_item(PyObject *mp, PyObject *key, PyObject *item, Py_hash_t hash) {
  return _PyDict_SetItem_KnownHash_LockHeld(reinterpret_cast<PyDictObject *>(mp), key, item, hash);
}
#else
inline int new_dict_set_item(PyObject *mp, PyObject *key, PyObject *item, Py_hash_t hash) {
  return _PyDict_SetItem_KnownHash(mp, key, item, hash);
}
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
// freed and its address reused while it is in the cache. A key enters the
// cache the second time it is seen (seen holds no reference: it only
// remembers an address), so that keys seen once cost little.
constexpr size_t ENCODED_KEY_CACHE_SIZE = 1024; // power of two
constexpr size_t ENCODED_KEY_MAX = 64;          // bytes of text
struct EncodedKey {
  PyObject *key = nullptr;
  PyObject *seen = nullptr;
  uint32_t len = 0;
  char text[ENCODED_KEY_MAX];
};

struct ThreadParser {
  parser *ptr = nullptr;
  simdjson::dom::document *spare = nullptr;
  KeyCacheEntry key_cache[KEY_CACHE_SIZE]{};
  KeyCacheEntry value_cache[KEY_CACHE_SIZE]{};
  EncodedKey *encoded_keys = nullptr; // allocated on first use by dumpb
  bool cleanup_registered = false;    // see register_thread_cleanup
  parser *chunk_parser = nullptr;     // for the chunks of parallel streams
  bool chunk_parser_busy = false;
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
    if (!chunk_parser_busy) {
      delete chunk_parser;
      chunk_parser = nullptr;
    }
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

// Frees this thread's parser, documents and caches (release()).
void release_thread_state() {
  delete g_thread_parser.ptr;
  g_thread_parser.ptr = nullptr;
  if (!g_thread_parser.chunk_parser_busy) {
    delete g_thread_parser.chunk_parser;
    g_thread_parser.chunk_parser = nullptr;
  }
  delete g_thread_parser.spare;
  g_thread_parser.spare = nullptr;
  clear_cache(g_thread_parser.key_cache);
  clear_cache(g_thread_parser.value_cache);
  g_thread_parser.clear_encoded_keys();
}

// The thread-local destructor runs after Python has deleted the thread
// state of an exiting thread, too late to release the cached strings. So
// each thread puts a capsule in its thread state's dict: Python clears that
// dict while the thread state is still valid, and the capsule's destructor
// releases the caches then. It does so only on its own thread (a thread
// state can be cleared from another thread, at finalization).
const char THREAD_CAPSULE[] = "fastsimdjson.thread_state";

void thread_cleanup(PyObject *capsule) {
  if (PyCapsule_GetPointer(capsule, THREAD_CAPSULE) == &g_thread_parser) {
    g_thread_parser.cleanup_registered = false;
    if (caches_can_decref()) {
      release_thread_state();
    }
  }
}

FSJ_NOINLINE void register_thread_cleanup_slow() {
  g_thread_parser.cleanup_registered = true;
  PyObject *dict = PyThreadState_GetDict(); // borrowed
  PyObject *capsule = dict ? PyCapsule_New(&g_thread_parser, THREAD_CAPSULE, thread_cleanup) : nullptr;
  if (capsule == nullptr || PyDict_SetItemString(dict, THREAD_CAPSULE, capsule) < 0) {
    PyErr_Clear(); // without it, the caches of an exiting thread leak, as before
  }
  Py_XDECREF(capsule);
}

inline void register_thread_cleanup() {
  if (!g_thread_parser.cleanup_registered) {
    register_thread_cleanup_slow();
  }
}

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
  register_thread_cleanup(); // the entry holds a reference: free it at thread exit
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
      int rc = new_dict_set_item(dict, key, v, reinterpret_cast<PyASCIIObject *>(key)->hash);
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
  register_thread_cleanup();
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
  bool copied; // made by copy_document, freed by free_copy
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

// Tape index of the value of the first field named key in the object at
// start, or 0.
size_t find_key_in(const uint64_t *tape, const uint8_t *strings, size_t start,
                   const char *key, size_t len) {
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

size_t find_key(DocumentObject *owner, size_t start, const char *key,
                size_t len) {
  return find_key_in(tape_of(owner), owner->doc->string_buf.get(), start, key, len);
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

// --- fields= ------------------------------------------------------------------
// loads_many(data, fields=...) and map_many(fn, data, fields=...): instead of
// each document, the values at some JSON Pointers, found on the tape. Only
// these values become Python objects.

struct FieldStep {
  std::string key;
  size_t index;    // the token as an array index, or SIZE_MAX
  PyObject *name;  // the token as a str (documents that json decoded)
};

struct Fields {
  std::vector<std::vector<FieldStep>> paths;
  std::vector<size_t> shared; // leading steps in common with the previous path
  size_t depth = 0;           // steps of the longest path
  bool single = false;        // fields was one str: the value, not a tuple
  PyObject *dflt = nullptr;   // the value of a missing field

  ~Fields() {
    for (auto &p : paths) {
      for (auto &s : p) {
        Py_XDECREF(s.name);
      }
    }
    Py_XDECREF(dflt);
  }
};

// Appends the steps of the JSON Pointer `arg` to `path`.
bool compile_pointer(PyObject *arg, std::vector<FieldStep> &path) {
  Py_ssize_t plen;
  const char *p = PyUnicode_Check(arg) ? PyUnicode_AsUTF8AndSize(arg, &plen) : nullptr;
  if (p == nullptr) {
    if (!PyErr_Occurred()) {
      PyErr_Format(PyExc_TypeError, "fields must be JSON pointers (str), not %.200s",
                   Py_TYPE(arg)->tp_name);
    }
    return false;
  }
  if (plen > 0 && p[0] != '/') {
    PyErr_Format(PyExc_ValueError, "invalid JSON pointer: %R", arg);
    return false;
  }
  const char *end = p + plen;
  for (const char *cur = p; cur < end;) {
    cur++; // '/'
    const char *stop = static_cast<const char *>(memchr(cur, '/', size_t(end - cur)));
    if (stop == nullptr) {
      stop = end;
    }
    std::string token(cur, size_t(stop - cur));
    cur = stop;
    for (size_t t = token.find('~'); t != std::string::npos; t = token.find('~', t + 1)) {
      if (t + 1 >= token.size() || (token[t + 1] != '0' && token[t + 1] != '1')) {
        PyErr_Format(PyExc_ValueError, "invalid JSON pointer: %R", arg);
        return false;
      }
      token.replace(t, 2, token[t + 1] == '0' ? "~" : "/");
    }
    bool digits = !token.empty() && (token.size() == 1 || token[0] != '0') &&
                  token.size() <= 18;
    size_t k = 0;
    for (char ch : token) {
      digits = digits && ch >= '0' && ch <= '9';
      k = k * 10 + size_t(ch - '0');
    }
    PyObject *name = PyUnicode_FromStringAndSize(token.data(), Py_ssize_t(token.size()));
    if (name == nullptr) {
      return false;
    }
    path.push_back(FieldStep{std::move(token), digits ? k : SIZE_MAX, name});
  }
  return true;
}

// The fields of `spec` (a JSON Pointer, or a list or tuple of them), or
// nullptr with an exception.
Fields *compile_fields(PyObject *spec, PyObject *dflt) {
  Fields *f = new (std::nothrow) Fields();
  if (f == nullptr) {
    PyErr_NoMemory();
    return nullptr;
  }
  f->dflt = Py_NewRef(dflt != nullptr ? dflt : Py_None);
  f->single = PyUnicode_Check(spec);
  PyObject *seq = f->single ? PyTuple_Pack(1, spec)
                  : PyList_Check(spec) || PyTuple_Check(spec)
                      ? PySequence_Tuple(spec)
                      : nullptr;
  if (seq == nullptr) {
    if (!PyErr_Occurred()) {
      PyErr_Format(PyExc_TypeError,
                   "fields must be a JSON pointer (str) or a list or tuple of them, not %.200s",
                   Py_TYPE(spec)->tp_name);
    }
    delete f;
    return nullptr;
  }
  for (Py_ssize_t n = 0; n < PyTuple_GET_SIZE(seq); n++) {
    f->paths.emplace_back();
    if (!compile_pointer(PyTuple_GET_ITEM(seq, n), f->paths.back())) {
      Py_DECREF(seq);
      delete f;
      return nullptr;
    }
    const std::vector<FieldStep> &path = f->paths.back();
    size_t common = 0;
    if (n > 0) {
      const std::vector<FieldStep> &prev = f->paths[size_t(n) - 1];
      while (common < path.size() && common < prev.size() &&
             path[common].key == prev[common].key) {
        common++;
      }
    }
    f->shared.push_back(common);
    f->depth = std::max(f->depth, path.size());
  }
  Py_DECREF(seq);
  return f;
}

// The fields of the document on the tape (root at index 1).
PyObject *extract_fields(const document &doc, const Fields &f) {
  const uint64_t *tape = doc.tape.get();
  const uint8_t *strings = doc.string_buf.get();
  Builder b(doc);
  size_t n = f.paths.size();
  PyObject *tuple = f.single ? nullptr : PyTuple_New(Py_ssize_t(n));
  if (!f.single && tuple == nullptr) {
    return nullptr;
  }
  // pos[d]: tape index reached after d steps of the previous path, which
  // reached `reached` steps; paths that share a prefix walk it once.
  size_t stack[16];
  std::vector<size_t> heap;
  size_t *pos = stack;
  if (f.depth + 1 > 16) {
    heap.resize(f.depth + 1);
    pos = heap.data();
  }
  pos[0] = 1;
  size_t reached = 0;
  for (size_t p = 0; p < n; p++) {
    const std::vector<FieldStep> &path = f.paths[p];
    size_t d = std::min(f.shared[p], reached);
    size_t i = pos[d];
    for (; d < path.size(); d++) {
      uint8_t type = uint8_t(tape[i] >> 56);
      const FieldStep &s = path[d];
      if (type == '{') {
        i = find_key_in(tape, strings, i, s.key.data(), s.key.size());
      } else if (type == '[' && s.index != SIZE_MAX) {
        i = walk_to(tape, i + 1, 0, s.index, close_of(tape, i));
      } else {
        i = 0;
      }
      if (i == 0) {
        break;
      }
      pos[d + 1] = i;
    }
    reached = d;
    PyObject *v;
    if (d == path.size()) {
      v = b.build(i);
      if (v == nullptr) {
        Py_XDECREF(tuple);
        return nullptr;
      }
    } else {
      v = Py_NewRef(f.dflt);
    }
    if (f.single) {
      return v;
    }
    PyTuple_SET_ITEM(tuple, Py_ssize_t(p), v);
  }
  return tuple;
}

PyObject *extract_fields_gc_paused(const document &doc, const Fields &f) {
#ifndef Py_GIL_DISABLED
  int gc_was_enabled = PyGC_Disable();
#endif
  PyObject *res = extract_fields(doc, f);
#ifndef Py_GIL_DISABLED
  if (gc_was_enabled) {
    PyGC_Enable();
  }
#endif
  return res;
}

// The fields of a document that json decoded into Python objects.
PyObject *extract_fields_py(PyObject *value, const Fields &f) {
  size_t n = f.paths.size();
  PyObject *tuple = f.single ? nullptr : PyTuple_New(Py_ssize_t(n));
  if (!f.single && tuple == nullptr) {
    return nullptr;
  }
  for (size_t p = 0; p < n; p++) {
    PyObject *v = value; // borrowed
    for (const FieldStep &s : f.paths[p]) {
      if (PyDict_Check(v)) {
        v = PyDict_GetItemWithError(v, s.name);
        if (v == nullptr && PyErr_Occurred()) {
          Py_XDECREF(tuple);
          return nullptr;
        }
      } else if (PyList_Check(v) && s.index != SIZE_MAX &&
                 s.index < size_t(PyList_GET_SIZE(v))) {
        v = PyList_GET_ITEM(v, Py_ssize_t(s.index));
      } else {
        v = nullptr;
      }
      if (v == nullptr) {
        break;
      }
    }
    v = Py_NewRef(v != nullptr ? v : f.dflt);
    if (f.single) {
      return v;
    }
    PyTuple_SET_ITEM(tuple, Py_ssize_t(p), v);
  }
  return tuple;
}

// --- Document ---------------------------------------------------------------

void free_copy(document *d);

void document_dealloc(PyObject *self) {
  PyTypeObject *tp = Py_TYPE(self);
  DocumentObject *owner = reinterpret_cast<DocumentObject *>(self);
  if (owner->copied) {
    free_copy(owner->doc);
  } else {
    give_back(owner->doc);
  }
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

// The root of document d, which the returned view takes over (d is a copy
// from copy_document when `copied`).
PyObject *view_of(document *d, bool copied = false) {
  DocumentObject *owner = PyObject_New(DocumentObject, DocumentType);
  if (owner == nullptr) {
    if (copied) {
      free_copy(d);
    } else {
      give_back(d);
    }
    return nullptr;
  }
  owner->doc = d;
  owner->copied = copied;
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
#ifdef Py_GIL_DISABLED
  PyObject *item = PyList_GetItemRef(list, i); // another thread may change it
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
  // With as_bytes, the output is written into a bytes object (no final copy).
  bool as_bytes = false;
  PyObject *bytes = nullptr;
  explicit OutBuf(bool b = false) : as_bytes(b) {}
  OutBuf(const OutBuf &) = delete;
  OutBuf &operator=(const OutBuf &) = delete;
  ~OutBuf() {
    if (as_bytes) {
      Py_XDECREF(bytes);
    } else {
      free(buf);
    }
  }
  FSJ_NOINLINE void grow(size_t n) {
    // As orjson: a bytes object of one page, doubled as needed (the sizes
    // matter to glibc, which may move a large buffer at each growth).
    size_t c = cap == 0 ? 4096 - sizeof(PyBytesObject) : cap;
    while (c < len + n + 1) {
      c *= 2;
    }
    if (as_bytes) {
      if (bytes == nullptr) {
        bytes = PyBytes_FromStringAndSize(nullptr, Py_ssize_t(c));
      } else if (_PyBytes_Resize(&bytes, Py_ssize_t(c)) < 0) {
        bytes = nullptr;
      }
      if (bytes == nullptr) {
        throw std::bad_alloc();
      }
      buf = PyBytes_AS_STRING(bytes);
    } else {
      char *p = static_cast<char *>(realloc(buf, c));
      if (p == nullptr) {
        throw std::bad_alloc();
      }
      buf = p;
    }
    cap = c;
  }
  // The bytes written (as_bytes only); the buffer gives it up.
  PyObject *take_bytes() {
    if (bytes == nullptr) {
      return PyBytes_FromStringAndSize(nullptr, 0);
    }
    if (len >= cap / 2) {
      // At least half full: keep the allocation, as orjson does. Shrinking a
      // large buffer lowers glibc's threshold for mmap, and the next large
      // output then moves to fresh pages at each growth.
      Py_SET_SIZE(bytes, Py_ssize_t(len));
      buf[len] = '\0';
    } else if (_PyBytes_Resize(&bytes, Py_ssize_t(len)) < 0) {
      bytes = nullptr;
      return nullptr;
    }
    PyObject *r = bytes;
    bytes = nullptr;
    return r;
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

// The 8 decimal digits of v < 10^8 as ASCII, most significant first in
// memory, computed in parallel within a 64-bit word.
inline uint64_t eight_digits(uint32_t v) {
  uint64_t merged = uint64_t(v / 10000) | (uint64_t(v % 10000) << 32);
  uint64_t hundreds = ((merged * 10486) >> 20) & 0x0000007F0000007FULL;
  uint64_t pairs = hundreds | ((merged - hundreds * 100) << 16);
  uint64_t tens = ((pairs * 103) >> 10) & 0x000F000F000F000FULL;
  uint64_t digits = tens | ((pairs - tens * 10) << 8);
  return digits + 0x3030303030303030ULL;
}

// Number of decimal digits of v > 0.
inline int decimal_digits(uint64_t v) {
  static const uint64_t powers[] = {
      1ULL, 10ULL, 100ULL, 1000ULL, 10000ULL, 100000ULL, 1000000ULL, 10000000ULL,
      100000000ULL, 1000000000ULL, 10000000000ULL, 100000000000ULL, 1000000000000ULL,
      10000000000000ULL, 100000000000000ULL, 1000000000000000ULL,
      10000000000000000ULL, 100000000000000000ULL, 1000000000000000000ULL,
      10000000000000000000ULL};
  int bits = 64 - clz64(v | 1);
  int guess = (bits * 1233) >> 12; // log10(2) ~ 1233 / 4096
  return guess + (v >= powers[guess]);
}

// A finite double as Python's repr writes it, or as orjson does. dragonbox
// gives the shortest digits that round-trip, the same digits as repr
// (checked on millions of values). repr writes them in fixed notation when
// the decimal exponent is in (-4, 16], and as d.ddde+XX otherwise (at least
// two exponent digits); orjson uses fixed notation from one more decade
// down, (-5, 16], and does not pad the exponent (1e-6).
// Stores 24 bytes at dst: the digit field w (3 words of ASCII digits, then
// filler) starting at its byte s, shifted in registers. The digits are never
// read back from memory: a load spanning several recent stores would stall.
inline void store_digits(char *dst, const uint64_t *w, unsigned s) {
  unsigned q = s >> 3, r = (s & 7) * 8;
  uint64_t a0 = w[q], a1 = w[q + 1], a2 = w[q + 2], a3 = w[q + 3];
  uint64_t o0 = r ? (a0 >> r) | (a1 << (64 - r)) : a0;
  uint64_t o1 = r ? (a1 >> r) | (a2 << (64 - r)) : a1;
  uint64_t o2 = r ? (a2 >> r) | (a3 << (64 - r)) : a2;
  memcpy(dst, &o0, 8);
  memcpy(dst + 8, &o1, 8);
  memcpy(dst + 16, &o2, 8);
}

template <bool Orjson>
FSJ_NOINLINE void append_float_layout(OutBuf &out, double v) {
  char *p = out.reserve(64); // writes overshoot the result: reserve the room
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
  // The shortest digits that round-trip (zmij: the digits of repr, possibly
  // followed by zeros), as a field of 24 ASCII digits in three words. The
  // first significant digit is at byte z; trailing '0's are dropped by
  // counting them in the words, not by dividing.
  zmij::dec_fp<> dec = zmij::to_decimal(v);
  uint64_t sig = dec.sig;
  const uint64_t zeros = 0x3030303030303030ULL;
  const uint64_t w[7] = {eight_digits(uint32_t(sig / 10000000000000000ULL)),
                         eight_digits(uint32_t((sig / 100000000) % 100000000)),
                         eight_digits(uint32_t(sig % 100000000)), zeros, zeros, zeros, zeros};
  int nd = decimal_digits(sig);
  // Trailing '0' digits: the last digit is the most significant byte of w[2].
  int tz;
  if (uint64_t x = w[2] ^ zeros) {
    tz = clz64(x) / 8;
  } else if (uint64_t y = w[1] ^ zeros) {
    tz = 8 + clz64(y) / 8;
  } else {
    tz = 16 + clz64(w[0] ^ zeros) / 8;
  }
  unsigned z = unsigned(24 - nd);
  int decpt = nd + dec.exp; // v = 0.d * 10^decpt
  nd -= tz;
  if (decpt > (Orjson ? -5 : -4) && decpt <= 16) {
    if (decpt <= 0) {
      memcpy(p, "0.000000", 8); // "0." and up to four zeros
      p += 2 - decpt;
      store_digits(p, w, z);
      p += nd;
    } else if (decpt >= nd) {
      store_digits(p, w, z); // the digits, then zeros from the filler
      p += decpt;
      memcpy(p, ".0", 2);
      p += 2;
    } else {
      store_digits(p, w, z);
      p += decpt;
      *p++ = '.';
      store_digits(p, w, z + unsigned(decpt));
      p += nd - decpt;
    }
  } else {
    // d.ddd: the digits one byte further, then the first moved before '.'.
    store_digits(p + 1, w, z);
    p[0] = p[1];
    if (nd > 1) {
      p[1] = '.';
      p += nd + 1;
    } else {
      p += 1;
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

// zmij writes the digits of repr: fixed notation for 1e-4 <= |v| < 1e16,
// d.ddde+XX otherwise. Integral values lack repr's ".0"; orjson also writes
// 1e-5 <= |v| < 1e-4 in fixed notation (append_float_layout) and does not pad
// a negative exponent (1e-6, not 1e-06). The bounds are compared as doubles:
// the shortest digits of a double below 1e-5 (or 1e-4) are below it too.
template <bool Orjson>
inline void append_float(OutBuf &out, double v) {
  double a = std::fabs(v);
  if (Orjson && a >= 1e-5 && a < 1e-4) {
    append_float_layout<true>(out, v);
    return;
  }
  char *p = out.reserve(64); // zmij needs 34 bytes, ".0" 2 more
  char *e = zmij::detail::write(p, v);
  if (a < 1e16 && a == double(int64_t(a))) {
    memcpy(e, ".0", 2);
    e += 2;
  } else if (Orjson && e - p >= 4 && e[-4] == 'e' && e[-3] == '-' && e[-2] == '0') {
    e[-2] = e[-1];
    e--;
  }
  out.len += size_t(e - p);
}

const char DIGIT_PAIRS[] =
    "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
    "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

// Decimal digits by magnitude, from pairs of digits (as itoap, which orjson
// uses): v < 10^4 in one to four digits, wider values in chunks of exactly
// four or eight digits.
inline char *write_u32_small(char *p, uint32_t v) { // v < 10^4
  if (v < 100) {
    if (v < 10) {
      *p = char('0' + v);
      return p + 1;
    }
    memcpy(p, DIGIT_PAIRS + 2 * v, 2);
    return p + 2;
  }
  uint32_t hi = v / 100, lo = v % 100;
  if (v < 1000) {
    *p = char('0' + hi);
    memcpy(p + 1, DIGIT_PAIRS + 2 * lo, 2);
    return p + 3;
  }
  memcpy(p, DIGIT_PAIRS + 2 * hi, 2);
  memcpy(p + 2, DIGIT_PAIRS + 2 * lo, 2);
  return p + 4;
}

inline char *write_u32_eight(char *p, uint32_t v) { // v < 10^8
  if (v < 10000) {
    return write_u32_small(p, v);
  }
  p = write_u32_small(p, v / 10000);
  uint32_t lo = v % 10000;
  memcpy(p, DIGIT_PAIRS + 2 * (lo / 100), 2);
  memcpy(p + 2, DIGIT_PAIRS + 2 * (lo % 100), 2);
  return p + 4;
}

inline char *write_u64(char *p, uint64_t v) {
  if (v < 100000000) {
    return write_u32_eight(p, uint32_t(v));
  }
  uint64_t low;
  if (v < 10000000000000000ULL) {
    p = write_u32_eight(p, uint32_t(v / 100000000));
  } else {
    uint64_t hi = v / 100000000;
    p = write_u32_small(p, uint32_t(hi / 100000000)); // at most 1844
    low = eight_digits(uint32_t(hi % 100000000));
    memcpy(p, &low, 8);
    p += 8;
  }
  low = eight_digits(uint32_t(v % 100000000));
  memcpy(p, &low, 8);
  return p + 8;
}

inline void append_u64(OutBuf &out, uint64_t v) {
  char *p = out.reserve(20);
  out.len += size_t(write_u64(p, v) - p);
}

inline void append_i64(OutBuf &out, long long v) {
  char *p = out.reserve(21);
  char *q = p;
  *q = '-';
  q += v < 0;
  uint64_t u = v < 0 ? uint64_t(0) - uint64_t(v) : uint64_t(v);
  out.len += size_t(write_u64(q, u) - p);
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

// The high bit of each byte of w that may need an escape: exact up to the
// first that does (a borrow may mark the bytes above it); bytes >= 0x80
// never do.
inline uint64_t escape_bits(uint64_t w, bool escape_del) {
  const uint64_t ones = 0x0101010101010101ULL;
  const uint64_t highs = 0x8080808080808080ULL;
  uint64_t lt20 = (w - ones * 0x20) & ~w;
  uint64_t q = w ^ (ones * '"');
  uint64_t b = w ^ (ones * '\\');
  uint64_t eq = ((q - ones) & ~q) | ((b - ones) & ~b);
  if (escape_del) {
    uint64_t d = w ^ (ones * 0x7F);
    eq |= (d - ones) & ~d;
  }
  return (lt20 | eq) & highs;
}

// True when one of the 8 bytes in w may need an escape (exact when none
// does).
inline bool word_needs_escape(uint64_t w, bool escape_del) {
  return escape_bits(w, escape_del) != 0;
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

// The escapes of the ASCII characters that need one, as 8 bytes to copy
// (the text, then padding) and the length in the last byte.
struct EscapeTable {
  char entry[128][8] = {};
  constexpr EscapeTable() {
    for (int c = 0; c < 128; c++) {
      char *e = entry[c];
      const char *hex = "0123456789abcdef";
      char short_form = c == '"' ? '"' : c == '\\' ? '\\' : c == '\b' ? 'b' : c == '\f' ? 'f'
                        : c == '\n' ? 'n' : c == '\r' ? 'r' : c == '\t' ? 't' : 0;
      if (short_form != 0) {
        e[0] = '\\';
        e[1] = short_form;
        e[7] = 2;
      } else if (c < 0x20 || c == 0x7F) {
        e[0] = '\\';
        e[1] = 'u';
        e[2] = '0';
        e[3] = '0';
        e[4] = hex[c >> 4];
        e[5] = hex[c & 0xF];
        e[7] = 6;
      }
    }
  }
};
constexpr EscapeTable ESCAPES;

// Writes the escape of the ASCII character c (one that needs it) at p, with
// room for 8 bytes; returns the new end.
inline char *write_escaped_ascii(char *p, uint8_t c) {
  memcpy(p, ESCAPES.entry[c], 8);
  return p + ESCAPES.entry[c][7];
}

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
    p = write_escaped_ascii(p, uint8_t(s[i]));
    i++;
  }
#endif
  // The tail (under 16 bytes) as two overlapping words: fixed-size loads and
  // stores, no call to memcpy.
  size_t n = len - i;
  if (n >= 8) {
    uint64_t a = load_u64(s + i), b = load_u64(s + len - 8);
    if (!word_needs_escape(a, escape_del) && !word_needs_escape(b, escape_del)) {
      if (n > 8) { // the second word may overlap the first: store it first
        memcpy(p + n - 8, &b, 8);
      }
      memcpy(p, &a, 8);
      if (n <= 8) {
        memcpy(p + n - 8, &b, 8);
      }
      return p + n;
    }
  } else if (n >= 4) {
    uint32_t a, b;
    memcpy(&a, s + i, 4);
    memcpy(&b, s + len - 4, 4);
    if (!word_needs_escape(uint64_t(a) | (uint64_t(b) << 32), escape_del)) {
      memcpy(p + n - 4, &b, 4);
      memcpy(p, &a, 4);
      return p + n;
    }
  }
  for (; i < len; i++) {
    uint8_t c = uint8_t(s[i]);
    if (byte_needs_escape(c, escape_del)) {
      p = write_escaped_ascii(p, c);
    } else {
      *p++ = char(c);
    }
  }
  return p;
}

// With AVX-512 (x64, detected at run time), as orjson: 32 bytes at a time,
// and the tail in one masked load (masked bytes are not read, so it cannot
// fault). Stores 32 bytes at a time: p must have room for 6 * len + 32
// bytes, and must not overlap s.
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#define FSJ_AVX512 1
bool has_avx512 = false;

// AVX-512F, BW and VL, with the vector and mask registers enabled by the
// operating system (macOS enables them on first use: the SSE2 code runs).
bool detect_avx512() {
  unsigned a, b, c, d;
  if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & (1u << 27))) { // OSXSAVE
    return false;
  }
  unsigned xcr0_lo, xcr0_hi;
  __asm__("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
  if ((xcr0_lo & 0xE6) != 0xE6) { // SSE, AVX, opmask and ZMM state
    return false;
  }
  if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) {
    return false;
  }
  return (b & (1u << 16)) && (b & (1u << 30)) && (b & (1u << 31)); // F, BW, VL
}

__attribute__((target("avx512f,avx512bw,avx512vl"))) FSJ_NOINLINE char *
escape_into_avx512(char *p, const char *s, size_t len, bool escape_del) {
  const __m256i backslash = _mm256_set1_epi8('\\'), quote = _mm256_set1_epi8('"');
  const __m256i space = _mm256_set1_epi8(0x20), del = _mm256_set1_epi8(0x7F);
  for (;;) {
    bool full = len >= 32;
    __mmask32 live = full ? ~__mmask32(0) : __mmask32((uint32_t(1) << len) - 1);
    __m256i v = _mm256_maskz_loadu_epi8(live, s);
    _mm256_storeu_si256(reinterpret_cast<__m256i *>(p), v);
    uint32_t m = _mm256_cmpeq_epi8_mask(v, backslash) | _mm256_cmpeq_epi8_mask(v, quote) |
                 _mm256_cmplt_epu8_mask(v, space);
    if (escape_del) {
      m |= _mm256_cmpeq_epi8_mask(v, del);
    }
    m &= live;
    if (m == 0) {
      if (!full) {
        return p + len;
      }
      p += 32;
      s += 32;
      len -= 32;
      continue;
    }
    size_t k = size_t(ctz64(m));
    p = write_escaped_ascii(p + k, uint8_t(s[k]));
    s += k + 1;
    len -= k + 1;
  }
}
#endif

// escape_into, with AVX-512 when available: p must have room for
// 6 * len + 32 bytes and must not overlap s.
inline char *escape_into_wide(char *p, const char *s, size_t len, bool escape_del) {
#if FSJ_AVX512
  if (has_avx512) {
    return escape_into_avx512(p, s, len, escape_del);
  }
#endif
  return escape_into(p, s, len, escape_del);
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
      char *p = out.reserve(6 * len + 34);
      *p++ = '"';
      p = escape_into_wide(p, static_cast<const char *>(PyUnicode_DATA(s)), len, ensure_ascii);
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
  OutBuf out{true};
  EncodedKey *keys = g_thread_parser.encoded_keys; // fetched once: TLS is slow
  PyObject *default_fn = nullptr; // borrowed
  unsigned long opts = 0;
  int depth = 0;         // containers being encoded
  int default_calls = 0; // nested calls to default
  // With borrowed, the items of containers are not referenced, as with
  // orjson: fast, and safe as long as no Python code runs, as none does for
  // the exact types. Python code (default, enum values, dict subclasses)
  // sets restart instead, and dumpb encodes again without borrowed.
  bool borrowed = false;
  bool restart = false;

  int fail(const char *msg) {
    PyErr_SetString(PyExc_TypeError, msg);
    return -1;
  }

  // Before running Python code: -1 (no exception) to start again when the
  // items are borrowed, else 0.
  int python_may_run() {
    if (borrowed) {
      restart = true;
      return -1;
    }
    return 0;
  }

  // As orjson: an instance of a class made by enum.EnumType.
  static bool is_enum(PyObject *o) {
    return reinterpret_cast<PyObject *>(Py_TYPE(o))->ob_type == Py_TYPE(EnumType);
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
#if FSJ_LITTLE_ENDIAN
      if (n < 8) {
        // One load: the 8 bytes ending with the terminating NUL, within the
        // object (the text follows a header of 40 bytes or more).
        uint64_t w = load_u64(p + n - 7) >> (8 * (7 - n));
        if ((escape_bits(w, false) & ((uint64_t(1) << (8 * n)) - 1)) == 0) {
          char *d = out.reserve(18);
          d[0] = '"';
          memcpy(d + 1, &w, 8);
          d[n + 1] = '"';
          out.len += size_t(n) + 2;
          return 0;
        }
      }
#endif
    } else {
      p = PyUnicode_AsUTF8AndSize(s, &n); // cached on the str, as orjson does
      if (p == nullptr) {
        PyErr_Clear();
        return fail("str is not valid UTF-8: surrogates not allowed");
      }
    }
    char *w = out.reserve(6 * size_t(n) + 34);
    *w++ = '"';
    w = escape_into_wide(w, p, size_t(n), false);
    *w++ = '"';
    out.len = size_t(w - out.buf);
    return 0;
  }

  FSJ_NOINLINE int encode_int(PyObject *o) {
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

  static void encode_float_into(OutBuf &o, double v) {
    if (std::isfinite(v)) {
      append_float<true>(o, v);
    } else {
      o.append("null", 4);
    }
  }

  void encode_float(double v) { encode_float_into(out, v); }

  void key_separator() {
    if (opts & OPT_INDENT_2) {
      out.append(": ", 2);
    } else {
      out.push_back(':');
    }
  }

  // An exact str key with its separators, when its text is not in the cache
  // (or with indent). A key enters the cache the second time it is seen.
  FSJ_NOINLINE int encode_str_key_miss(PyObject *k, EncodedKey *e, bool first) {
    separate(first);
    size_t start = out.len;
    if (encode_str(k) < 0) {
      return -1;
    }
    size_t n = out.len - start;
    if (e == nullptr || e->key == k) {
      // No cache, or in the cache (with indent).
    } else if (e->seen != k) {
      e->seen = k;
    } else if (n <= ENCODED_KEY_MAX) {
      out.reserve(ENCODED_KEY_MAX); // the copy reads a whole entry
      Py_INCREF(k);
      Py_XDECREF(e->key);
      e->key = k;
      e->len = uint32_t(n);
      memcpy(e->text, out.buf + start, ENCODED_KEY_MAX);
    }
    key_separator();
    return 0;
  }

  // A dict entry without OPT_NON_STR_KEYS or OPT_SORT_KEYS: the key must be
  // an exact str (not a subclass, as with orjson).
  FSJ_ALWAYS_INLINE int encode_entry(PyObject *key, PyObject *value, bool first) {
    if (!PyUnicode_CheckExact(key)) {
      return fail("Dict key must be str");
    }
    EncodedKey *e = keys == nullptr ? nullptr
                                    : &keys[(reinterpret_cast<uintptr_t>(key) >> 4) &
                                            (ENCODED_KEY_CACHE_SIZE - 1)];
    if (e != nullptr && e->key == key && !(opts & OPT_INDENT_2)) {
      char *d = out.reserve(ENCODED_KEY_MAX + 2);
      *d = ',';
      d += !first;
      memcpy(d, e->text, ENCODED_KEY_MAX);
      d[e->len] = ':';
      out.len = size_t(d + e->len + 1 - out.buf);
    } else if (encode_str_key_miss(key, e, first) < 0) {
      return -1;
    }
    return encode(value);
  }

  static int utf8_of(PyObject *s, std::string &text) {
    Py_ssize_t n;
    const char *u = PyUnicode_AsUTF8AndSize(s, &n);
    if (u == nullptr) {
      PyErr_Clear();
      PyErr_SetString(PyExc_TypeError, "str is not valid UTF-8: surrogates not allowed");
      return -1;
    }
    text.assign(u, size_t(n));
    return 0;
  }

  // The text of a key other than an exact str, with OPT_NON_STR_KEYS. As
  // orjson: OPT_PASSTHROUGH_SUBCLASS does not apply, int keys may exceed 53
  // bits, and a float subclass is not accepted.
  int non_str_key(PyObject *k, std::string &text) {
    if (PyUnicode_Check(k)) {
      return utf8_of(k, text);
    }
    if (k == Py_None || k == Py_True || k == Py_False) {
      text = k == Py_None ? "null" : k == Py_True ? "true" : "false";
      return 0;
    }
    char buf[64];
    char *end;
    if (PyLong_Check(k)) {
      int overflow = 0;
      long long v = PyLong_AsLongLongAndOverflow(k, &overflow);
      if (overflow == 0) {
        if (v == -1 && PyErr_Occurred()) {
          return -1;
        }
        char *p = buf;
        *p = '-';
        p += v < 0;
        end = write_u64(p, v < 0 ? uint64_t(0) - uint64_t(v) : uint64_t(v));
      } else {
        unsigned long long u = overflow > 0 ? PyLong_AsUnsignedLongLong(k) : (unsigned long long)-1;
        if (u == (unsigned long long)-1 && (overflow < 0 || PyErr_Occurred())) {
          PyErr_Clear();
          return fail("Dict integer key must be within 64-bit range");
        }
        end = write_u64(buf, u);
      }
      text.assign(buf, size_t(end - buf));
      return 0;
    }
    if (PyFloat_CheckExact(k)) {
      OutBuf tmp;
      encode_float_into(tmp, PyFloat_AS_DOUBLE(k));
      text.assign(tmp.data(), tmp.size());
      return 0;
    }
    if (is_enum(k)) {
      if (python_may_run() < 0) {
        return -1;
      }
      PyObject *value = PyObject_GetAttrString(k, "value");
      if (value == nullptr) {
        return -1;
      }
      int rc = PyUnicode_CheckExact(value) ? utf8_of(value, text) : non_str_key(value, text);
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

  FSJ_NOINLINE int encode_list(PyObject *o) {
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
    if (borrowed) {
      PyObject **items = is_list ? reinterpret_cast<PyListObject *>(o)->ob_item
                                 : reinterpret_cast<PyTupleObject *>(o)->ob_item;
      for (Py_ssize_t i = 0; i < n; i++) {
        separate(i == 0);
        if (encode(items[i]) < 0) {
          return -1;
        }
      }
    } else {
#ifdef Py_GIL_DISABLED
      if (encode_items_locked(o, is_list) < 0) {
        return -1;
      }
#else
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
#endif
    }
    close(']');
    return 0;
  }

  // With OPT_NON_STR_KEYS or OPT_SORT_KEYS: the text of every key first (so
  // its errors come before those of the values, as with orjson), sorted by
  // UTF-8 with OPT_SORT_KEYS.
  FSJ_NOINLINE int encode_dict_collected(PyObject *o, PyObject *items) {
    struct Entry {
      std::string key;
      PyObject *value;
    };
    std::vector<Entry> entries;
    entries.reserve(size_t(PyDict_GET_SIZE(o)));
    Py_ssize_t pos = 0, i = 0;
    PyObject *key, *value;
    for (;;) {
      if (items != nullptr) {
        if (i == PyList_GET_SIZE(items)) {
          break;
        }
        PyObject *pair = PyList_GET_ITEM(items, i++);
        key = PyTuple_GET_ITEM(pair, 0);
        value = PyTuple_GET_ITEM(pair, 1);
      } else if (!PyDict_Next(o, &pos, &key, &value)) {
        break;
      }
      entries.push_back({std::string(), value});
      int rc = PyUnicode_CheckExact(key)              ? utf8_of(key, entries.back().key)
               : (opts & OPT_NON_STR_KEYS) != 0 ? non_str_key(key, entries.back().key)
                                                 : fail("Dict key must be str");
      if (rc < 0) {
        return -1;
      }
    }
    if (opts & OPT_SORT_KEYS) {
      std::stable_sort(entries.begin(), entries.end(),
                       [](const Entry &a, const Entry &b) { return a.key < b.key; });
    }
    for (size_t j = 0; j < entries.size(); j++) {
      separate(j == 0);
      const std::string &k = entries[j].key;
      char *w = out.reserve(6 * k.size() + 34);
      *w++ = '"';
      w = escape_into_wide(w, k.data(), k.size(), false);
      *w++ = '"';
      out.len = size_t(w - out.buf);
      key_separator();
      if (encode(entries[j].value) < 0) {
        return -1;
      }
    }
    return 0;
  }

#ifdef Py_GIL_DISABLED
  // Free-threaded Python: a dict or list is locked while its items are
  // encoded, and the items are borrowed when that is safe. Taking references
  // instead (a snapshot of the items) is slow on many threads: the records
  // of a list often share their keys and short strings, and the threads then
  // all update the same reference counts. Encoding an item that may lock
  // another object or run Python code can suspend the lock (another thread
  // may then change the container), so such an item is referenced first:
  // anything but an exact int, float, ASCII str, None, True or False.
  static bool borrowable(PyObject *v) {
    PyTypeObject *t = Py_TYPE(v);
    return (t == &PyUnicode_Type && PyUnicode_IS_COMPACT_ASCII(v)) || t == &PyLong_Type ||
           t == &PyFloat_Type || v == Py_None || v == Py_True || v == Py_False;
  }

  // References released when they go out of scope, also when the output
  // buffer throws std::bad_alloc.
  struct Refs {
    PyObject *a, *b;
    ~Refs() {
      Py_XDECREF(a);
      Py_XDECREF(b);
    }
  };

  // The lock is released before std::bad_alloc goes further.
  FSJ_NOINLINE int encode_entries_locked(PyObject *o) {
    int rc = 0;
    bool oom = false;
    Py_BEGIN_CRITICAL_SECTION(o);
    try {
      Py_ssize_t pos = 0;
      PyObject *key, *value;
      bool first = true;
      while (rc == 0 && PyDict_Next(o, &pos, &key, &value)) {
        if (borrowable(key) && borrowable(value)) {
          rc = encode_entry(key, value, first);
        } else {
          Refs r{Py_NewRef(key), Py_NewRef(value)};
          rc = encode_entry(key, value, first);
        }
        first = false;
      }
    } catch (const std::bad_alloc &) {
      oom = true;
    }
    Py_END_CRITICAL_SECTION();
    if (oom) {
      throw std::bad_alloc();
    }
    return rc;
  }

  // A tuple cannot change: its items live as long as it does.
  FSJ_NOINLINE int encode_items_locked(PyObject *o, bool is_list) {
    int rc = 0;
    if (!is_list) {
      for (Py_ssize_t i = 0; rc == 0 && i < PyTuple_GET_SIZE(o); i++) {
        separate(i == 0);
        rc = encode(PyTuple_GET_ITEM(o, i));
      }
      return rc;
    }
    bool oom = false;
    Py_BEGIN_CRITICAL_SECTION(o);
    try {
      for (Py_ssize_t i = 0; rc == 0 && i < PyList_GET_SIZE(o); i++) {
        PyObject *item = PyList_GET_ITEM(o, i);
        separate(i == 0);
        if (borrowable(item)) {
          rc = encode(item);
        } else {
          Refs r{Py_NewRef(item), nullptr};
          rc = encode(item);
        }
      }
    } catch (const std::bad_alloc &) {
      oom = true;
    }
    Py_END_CRITICAL_SECTION();
    if (oom) {
      throw std::bad_alloc();
    }
    return rc;
  }
#endif

  // Dict subclasses as dicts (their items, as stored), as with orjson.
  FSJ_NOINLINE int encode_dict(PyObject *o) {
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
    if (keys == nullptr) {
      register_thread_cleanup();
      keys = g_thread_parser.encoded_keys = new (std::nothrow) EncodedKey[ENCODED_KEY_CACHE_SIZE];
    }
    int rc = 0;
#ifdef Py_GIL_DISABLED
    if (!(opts & (OPT_NON_STR_KEYS | OPT_SORT_KEYS))) {
      rc = encode_entries_locked(o);
      if (rc == 0) {
        close('}');
      }
      return rc;
    }
#endif
    // Without borrowed (Python code may run, or another thread), a snapshot
    // holds the keys and values.
    PyObject *items = borrowed ? nullptr : PyDict_Items(o);
    if (!borrowed && items == nullptr) {
      return -1;
    }
    if (opts & (OPT_NON_STR_KEYS | OPT_SORT_KEYS)) {
      rc = encode_dict_collected(o, items);
    } else if (items != nullptr) {
      for (Py_ssize_t i = 0; rc == 0 && i < PyList_GET_SIZE(items); i++) {
        PyObject *pair = PyList_GET_ITEM(items, i);
        rc = encode_entry(PyTuple_GET_ITEM(pair, 0), PyTuple_GET_ITEM(pair, 1), i == 0);
      }
    } else {
      Py_ssize_t pos = 0;
      PyObject *key, *value;
      bool first = true;
      while (rc == 0 && PyDict_Next(o, &pos, &key, &value)) {
        rc = encode_entry(key, value, first);
        first = false;
      }
    }
    Py_XDECREF(items);
    if (rc == 0) {
      close('}');
    }
    return rc;
  }

  FSJ_NOINLINE int encode_default(PyObject *o) {
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

  // The common types inline, in the loops over containers; the others in
  // encode_other.
  FSJ_ALWAYS_INLINE int encode(PyObject *o) {
    PyTypeObject *t = Py_TYPE(o);
    if (t == &PyUnicode_Type) {
      return encode_str(o);
    }
    if (t == &PyLong_Type) {
#if PY_VERSION_HEX >= 0x030C0000
      if (PyUnstable_Long_IsCompact(reinterpret_cast<PyLongObject *>(o))) {
        // At most 30 bits: within the 53-bit range of OPT_STRICT_INTEGER.
        append_i64(out, PyUnstable_Long_CompactValue(reinterpret_cast<PyLongObject *>(o)));
        return 0;
      }
#endif
      return encode_int(o);
    }
    if (t == &PyFloat_Type) {
      encode_float(PyFloat_AS_DOUBLE(o));
      return 0;
    }
    if (o == Py_None) {
      memcpy(out.reserve(4), "null", 4);
      out.len += 4;
      return 0;
    }
    if (o == Py_True || o == Py_False) {
      memcpy(out.reserve(5), o == Py_True ? "true " : "false", 5);
      out.len += o == Py_True ? 4 : 5;
      return 0;
    }
    if (t == &PyDict_Type) {
      return encode_dict(o);
    }
    if (t == &PyList_Type || t == &PyTuple_Type) {
      return encode_list(o);
    }
    return encode_other(o);
  }

  // In the order of orjson: subclasses of str, int, list and dict (unless
  // OPT_PASSTHROUGH_SUBCLASS), enums, then default (subclasses of float and
  // tuple too).
  FSJ_NOINLINE int encode_other(PyObject *o) {
    if (!(opts & OPT_PASSTHROUGH_SUBCLASS)) {
      if (PyUnicode_Check(o)) {
        return encode_str(o);
      }
      if (PyLong_Check(o)) {
        return encode_int(o);
      }
      if (PyList_Check(o)) {
        return encode_list(o);
      }
      if (PyDict_Check(o)) {
        return encode_dict(o);
      }
    }
    if (is_enum(o)) {
      if (python_may_run() < 0) {
        return -1;
      }
      PyObject *value = PyObject_GetAttrString(o, "value");
      if (value == nullptr) {
        return -1;
      }
      int rc = encode(value);
      Py_DECREF(value);
      return rc;
    }
    if (default_fn != nullptr && python_may_run() < 0) {
      return -1;
    }
    return encode_default(o);
  }
};

// The flags of option= (None, or an int of orjson's OPT_* flags), as
// orjson.dumps checks them.
bool orjson_options(PyObject *option, unsigned long *opts) {
  *opts = 0;
  if (option == nullptr || option == Py_None) {
    return true;
  }
  if (!PyLong_Check(option)) {
    PyErr_SetString(PyExc_TypeError, "Invalid opts");
    return false;
  }
  long v = PyLong_AsLong(option);
  if ((v == -1 && PyErr_Occurred()) || v < 0 || (unsigned long)v > OPT_ALL) {
    PyErr_Clear();
    PyErr_SetString(PyExc_TypeError, "Invalid opts");
    return false;
  }
  *opts = (unsigned long)v;
  return true;
}

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
  if (dflt != nullptr) {
    // Any object, as orjson: calling one that is not callable (even None)
    // fails, and becomes the __cause__ of the error.
    e.default_fn = dflt;
  }
  if (!orjson_options(option, &e.opts)) {
    return nullptr;
  }
  int rc;
  try {
#ifndef Py_GIL_DISABLED
    e.borrowed = true;
#endif
    rc = e.encode(obj);
    if (rc < 0 && e.restart) {
      e.borrowed = e.restart = false;
      e.out.len = 0;
      e.depth = e.default_calls = 0;
      rc = e.encode(obj);
    }
    if (rc == 0 && (e.opts & OPT_APPEND_NEWLINE)) {
      e.out.push_back('\n');
    }
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  }
  if (rc < 0) {
    return nullptr;
  }
  return e.out.take_bytes();
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
  size_t text_len; // bytes of buf that `text` covers (len, or more for a chunk)
  bool no_fallback;   // a chunk of a parallel run: do not call json (see run_chunk)
  bool need_fallback; // set instead of calling json, with no_fallback
  bool borrowed_parser; // p is this thread's chunk parser (not deleted)
  Fields *fields;       // fields=: the values at these pointers, not the documents
  bool owns_fields;     // false in the chunks of a parallel run
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
                  : PyUnicode_DecodeUTF8(s->buf, Py_ssize_t(s->text_len), nullptr);
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

// A copy of the parser's current document, in one block from PyMem_Malloc
// (on an attached thread): the document, then its tape, then its strings.
// Streams on several threads make many of them at once, and the calling
// thread frees them later: with glibc's malloc, the threads' heaps grew and
// shrank at every call, and the system calls doing it are serialized.
// PyMem_Malloc is mimalloc on free-threaded builds, without this problem.
// The document's buffers are not its own: free_copy releases them first.
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
  size_t head = (sizeof(document) + 63) & ~size_t(63);
  size_t tape_bytes = n * sizeof(uint64_t);
  char *block = static_cast<char *>(
      PyMem_Malloc(head + tape_bytes + strings + simdjson::SIMDJSON_PADDING));
  if (block == nullptr) {
    return nullptr;
  }
  document *d = new (block) document();
  uint64_t *t = reinterpret_cast<uint64_t *>(block + head);
  uint8_t *str = reinterpret_cast<uint8_t *>(block + head + tape_bytes);
  memcpy(t, tape, tape_bytes);
  memcpy(str, src.string_buf.get(), strings);
  memset(str + strings, 0, simdjson::SIMDJSON_PADDING);
  d->tape.reset(t);
  d->string_buf.reset(str);
  return d;
}

void free_copy(document *d) {
  (void)d->tape.release(); // in the block, not allocated with new[]
  (void)d->string_buf.release();
  d->~document();
  PyMem_Free(d);
}

PyObject *stream_value(StreamObject *s) {
  if (s->fields != nullptr) {
    return extract_fields_gc_paused(s->p->doc, *s->fields);
  }
  if (!s->lazy) {
    return build_with_gc_paused(s->p->doc, 1);
  }
  document *d = copy_document(s->p->doc);
  return d == nullptr ? PyErr_NoMemory() : view_of(d, true);
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
  if (s->fields != nullptr) {
    PyObject *v = extract_fields_py(value, *s->fields);
    Py_DECREF(value);
    return v;
  }
  // As with parse(), a document that only json accepts is returned as plain
  // Python objects.
  return value;
}

// In a chunk of a parallel run, json is not called: the run continues in one
// thread from there, where json decides as it does without threads.
PyObject *stream_fallback_or_stop(StreamObject *s) {
  if (s->no_fallback) {
    s->need_fallback = true;
    return nullptr;
  }
  return stream_fallback(s);
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
      return skip_separators(s, s->last_end) < s->len ? stream_fallback_or_stop(s) : nullptr;
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
    return stream_fallback_or_stop(s);
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
  if (s->borrowed_parser) {
    g_thread_parser.chunk_parser_busy = false;
  } else {
    delete s->p;
  }
  if (s->owns_fields) {
    delete s->fields;
  }
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
  s->no_fallback = s->need_fallback = s->borrowed_parser = false;
  s->fields = nullptr;
  s->owns_fields = false;
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
  s->len = s->text_len = size_t(len);
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

// ---------------------------------------------------------------------------
// Streams over several threads (threads=N): the input is cut into chunks at
// document boundaries (after a newline; before a record separator for
// json_seq), and threads take the chunks in turn, each building the
// documents of its chunk (and calling fn on them, for map_many). Without
// the GIL (free-threaded Python), the threads run in parallel; with it, the
// calling thread does all the work.
//
// The result is that of one thread: at the first chunk where anything other
// than plain documents shows up (an error, a document that only json
// accepts), the documents before it are kept and the calling thread goes on
// from there as a stream without threads would. (fn may then have been
// called on documents after that point, in other threads.)
// ---------------------------------------------------------------------------

// A stream over [from, to) of the input of `whole`, sharing its buffer. A
// chunk of a parallel run uses this thread's chunk parser when it is free.
StreamObject *stream_over(StreamObject *whole, size_t from, size_t to, bool chunk = false) {
  StreamObject *s = PyObject_New(StreamObject, StreamType);
  if (s == nullptr) {
    return nullptr;
  }
  s->source = Py_NewRef(whole->source);
  s->copy = nullptr;
  s->text = nullptr;
  s->stream = nullptr;
  s->p = nullptr;
  new (&s->it) document_stream::iterator();
  s->buf = whole->buf;
  s->len = to;
  s->text_len = whole->len;
  s->batch_size = whole->batch_size;
  s->format = whole->format;
  s->lazy = whole->lazy;
  s->done = false;
  s->active = false;
  s->base = 0;
  s->last_end = from;
  s->prefix = whole->prefix;
  s->open = whole->open;
  s->close = whole->close;
  s->no_fallback = s->need_fallback = s->borrowed_parser = false;
  s->fields = whole->fields; // whole outlives its chunks
  s->owns_fields = false;
  if (chunk && !g_thread_parser.chunk_parser_busy) {
    if (g_thread_parser.chunk_parser == nullptr) {
      register_thread_cleanup();
      g_thread_parser.chunk_parser = new (std::nothrow) parser();
    }
    if (g_thread_parser.chunk_parser != nullptr) {
      s->p = g_thread_parser.chunk_parser;
      s->borrowed_parser = g_thread_parser.chunk_parser_busy = true;
    }
  }
  if (s->p == nullptr) {
    s->p = new (std::nothrow) parser();
  }
  if (s->p == nullptr) {
    Py_DECREF(reinterpret_cast<PyObject *>(s));
    PyErr_NoMemory();
    return nullptr;
  }
  s->p->number_as_string(true);
  // simdjson's streams run stage 1 of the next batch in a thread of their
  // own; the threads of a parallel run already use all the cores.
  s->p->threaded = !chunk;
  return s;
}

// Work that pool workers can help with: work() takes pieces of it until
// none is left. active counts the workers inside work(), guarded by the
// pool's mutex.
struct PoolTask {
  size_t active = 0;
  virtual void work() = 0;
  virtual ~PoolTask() = default;
};

struct ParallelRun : PoolTask {
  StreamObject *whole;
  PyObject *fn; // borrowed, or nullptr
  std::vector<size_t> bounds; // chunk i is [bounds[i], bounds[i + 1])
  struct Chunk {
    PyObject *items = nullptr; // list of the chunk's results
    bool failed = false;       // stopped before the end of the chunk
    PyObject *exc = nullptr;   // the exception fn raised, if it did
    size_t resume_at = 0;      // where the one-thread run goes on
  };
  std::vector<Chunk> chunks;
  std::atomic<size_t> next{0};
  std::atomic<size_t> first_failure{SIZE_MAX};

  void note_failure(size_t i) {
    size_t f = first_failure.load();
    while (i < f && !first_failure.compare_exchange_weak(f, i)) {
    }
  }

  // On an attached thread.
  void run_chunk(size_t i) {
    Chunk &c = chunks[i];
    c.resume_at = bounds[i];
    StreamObject *st = stream_over(whole, bounds[i], bounds[i + 1], true);
    c.items = st ? PyList_New(0) : nullptr;
    if (c.items == nullptr) {
      PyErr_Clear(); // the one-thread run will report it
      c.failed = true;
    }
    if (st != nullptr && c.items != nullptr) {
      st->no_fallback = true;
      while (first_failure.load(std::memory_order_relaxed) > i) {
        size_t before = st->last_end;
        PyObject *v = stream_next_locked(st);
        if (v == nullptr) {
          if (PyErr_Occurred() || st->need_fallback) {
            PyErr_Clear(); // reported again by the one-thread run
            c.failed = true;
            c.resume_at = before;
          }
          break;
        }
        c.resume_at = st->last_end;
        if (fn != nullptr) {
          PyObject *r = PyObject_CallOneArg(fn, v);
          Py_DECREF(v);
          if (r == nullptr) {
            PyObject *t, *val, *tb;
            PyErr_Fetch(&t, &val, &tb);
            PyErr_NormalizeException(&t, &val, &tb);
            if (tb != nullptr && val != nullptr) {
              PyException_SetTraceback(val, tb);
            }
            Py_XDECREF(t);
            Py_XDECREF(tb);
            c.exc = val;
            c.failed = true;
            break;
          }
          v = r;
        }
        int rc = PyList_Append(c.items, v);
        Py_DECREF(v);
        if (rc < 0) {
          PyErr_Clear();
          c.failed = true;
          break;
        }
      }
    }
    Py_XDECREF(reinterpret_cast<PyObject *>(st));
    if (c.failed) {
      note_failure(i);
    }
  }

  // Takes chunks until none is left, letting the garbage collector of
  // other threads run between chunks.
  void work() override {
    for (;;) {
      size_t i = next.fetch_add(1);
      if (i >= chunks.size() || i > first_failure.load()) {
        return;
      }
      run_chunk(i);
      Py_BEGIN_ALLOW_THREADS
      Py_END_ALLOW_THREADS
    }
  }
};

#ifdef Py_GIL_DISABLED
// Worker threads for the parallel streams, started once and kept: starting
// and ending threads (with their Python thread states) cost about as much
// as the parsing on many cores. Idle workers are detached from Python and
// wait for tasks: a task is a run to help with. A caller does the work of
// its run too; at the end, it takes back the tasks that no worker started
// and waits only for the workers that did start, so concurrent and nested
// runs cannot deadlock.
struct WorkerPool {
  std::mutex mu;
  std::condition_variable wake; // tasks, or stopping
  std::condition_variable done; // a worker finished a task, or exited
  std::deque<PoolTask *> tasks;
  PyInterpreterState *interp = nullptr;
  size_t workers = 0;
  size_t ready = 0; // workers with a thread state, waiting for tasks
  size_t exited = 0;
  bool stopping = false;
};
WorkerPool *g_pool = nullptr; // never freed: its threads may outlive anything
std::mutex g_pool_mu;

void pool_worker(WorkerPool *pool) {
  PyThreadState *ts = PyThreadState_New(pool->interp);
  {
    std::lock_guard<std::mutex> lk(pool->mu);
    pool->ready++;
  }
  pool->done.notify_all();
  for (;;) {
    PoolTask *run;
    {
      std::unique_lock<std::mutex> lk(pool->mu);
      pool->wake.wait(lk, [pool] { return pool->stopping || !pool->tasks.empty(); });
      if (pool->tasks.empty()) {
        break; // stopping
      }
      run = pool->tasks.front();
      pool->tasks.pop_front();
      run->active++;
    }
    if (ts != nullptr) {
      PyEval_RestoreThread(ts);
      run->work();
      PyEval_SaveThread();
    }
    {
      std::lock_guard<std::mutex> lk(pool->mu);
      run->active--;
    }
    pool->done.notify_all();
  }
  if (ts != nullptr) {
    PyEval_RestoreThread(ts);
    PyThreadState_Clear(ts);
    PyThreadState_DeleteCurrent();
  }
  {
    std::lock_guard<std::mutex> lk(pool->mu);
    pool->exited++;
  }
  pool->done.notify_all();
}

// The pool, with at least n workers (fewer if threads cannot be started).
// New workers create their thread states before any work is handed out:
// CPython may stop the world to make room for a thread state, which must
// then wait for every attached thread, including busy workers. So the
// caller waits, detached, for the new workers to be ready. (No lock is held
// while a thread waits for others.)
WorkerPool *get_pool(size_t n) {
  WorkerPool *pool;
  size_t target;
  {
    std::lock_guard<std::mutex> g(g_pool_mu);
    PyInterpreterState *interp = PyInterpreterState_Get();
    if (g_pool == nullptr) {
      g_pool = new (std::nothrow) WorkerPool();
      if (g_pool == nullptr) {
        return nullptr;
      }
      g_pool->interp = interp;
    }
    pool = g_pool;
    if (pool->interp != interp || pool->stopping) {
      return nullptr; // another interpreter, or exiting: no helpers
    }
    while (pool->workers < std::min<size_t>(n, 1024)) {
      try {
        std::thread(pool_worker, pool).detach();
      } catch (...) {
        break;
      }
      std::lock_guard<std::mutex> lk(pool->mu);
      pool->workers++;
    }
    target = pool->workers;
  }
  bool waiting;
  {
    std::lock_guard<std::mutex> lk(pool->mu);
    waiting = pool->ready < target;
  }
  if (waiting) {
    Py_BEGIN_ALLOW_THREADS
    std::unique_lock<std::mutex> lk(pool->mu);
    pool->done.wait(lk, [pool, target] { return pool->ready >= target; });
    Py_END_ALLOW_THREADS
  }
  return pool;
}

// atexit: the workers delete their thread states while the interpreter is
// still alive, then exit.
PyObject *stop_pool(PyObject *, PyObject *) {
  WorkerPool *pool;
  {
    std::lock_guard<std::mutex> g(g_pool_mu);
    pool = g_pool;
  }
  if (pool != nullptr) {
    Py_BEGIN_ALLOW_THREADS
    std::unique_lock<std::mutex> lk(pool->mu);
    pool->stopping = true;
    pool->wake.notify_all();
    pool->done.wait(lk, [pool] { return pool->exited == pool->workers; });
    Py_END_ALLOW_THREADS
  }
  Py_RETURN_NONE;
}

// Runs task->work() in the calling thread, helped by up to nthreads - 1
// pool workers; returns when the task is done.
void run_on_pool(PoolTask *task, size_t nthreads) {
  WorkerPool *pool = nthreads > 1 ? get_pool(nthreads - 1) : nullptr;
  if (pool != nullptr) {
    {
      std::lock_guard<std::mutex> lk(pool->mu);
      for (size_t t = 1; t < nthreads; t++) {
        pool->tasks.push_back(task);
      }
    }
    pool->wake.notify_all();
  }
  task->work(); // the calling thread too
  if (pool != nullptr) {
    Py_BEGIN_ALLOW_THREADS
    std::unique_lock<std::mutex> lk(pool->mu);
    for (auto it = pool->tasks.begin(); it != pool->tasks.end();) {
      it = *it == task ? pool->tasks.erase(it) : it + 1;
    }
    pool->done.wait(lk, [task] { return task->active == 0; });
    Py_END_ALLOW_THREADS
  }
}

#ifndef _WIN32
// A forked child has none of the workers: forget the pool (and its mutexes,
// which another thread may have held during the fork).
void pool_after_fork() {
  g_pool = nullptr;
  new (&g_pool_mu) std::mutex();
}
#endif
#endif

// Chunk boundaries: about `n` chunks, each ending just after a newline (or
// just before a record separator, for json_seq).
std::vector<size_t> chunk_bounds(const StreamObject *w, size_t n) {
  std::vector<size_t> b{0};
  size_t step = std::max<size_t>(w->len / std::max<size_t>(n, 1), 1 << 16);
  bool seq = w->format == simdjson::stream_format::json_sequence;
  for (size_t at = step; at < w->len; at = b.back() + step) {
    const void *hit = memchr(w->buf + at, seq ? '\x1e' : '\n', w->len - at);
    if (hit == nullptr) {
      break;
    }
    size_t cut = size_t(static_cast<const char *>(hit) - w->buf) + (seq ? 0 : 1);
    if (cut >= w->len) {
      break;
    }
    b.push_back(cut);
  }
  b.push_back(w->len);
  return b;
}

// All the results (documents, or fn of each) of the stream `whole`, as a
// list, using up to `threads` threads.
PyObject *run_stream(StreamObject *whole, PyObject *fn, size_t threads) {
  PyObject *result = PyList_New(0);
  if (result == nullptr) {
    return nullptr;
  }
  size_t resume_at = 0;
#ifdef Py_GIL_DISABLED
  if (threads > 1 && whole->len >= (size_t(1) << 17)) {
    ParallelRun run;
    run.whole = whole;
    run.fn = fn;
    run.bounds = chunk_bounds(whole, threads * 8);
    run.chunks.resize(run.bounds.size() - 1);
    // Starting a thread costs about as much as building 512 KB of documents.
    size_t nthreads = std::min({threads, run.chunks.size(), std::max<size_t>(whole->len >> 19, 1)});
    run_on_pool(&run, nthreads);
    size_t f = run.first_failure.load();
    size_t total = 0;
    for (size_t i = 0; i < run.chunks.size() && i <= f; i++) {
      total += run.chunks[i].items ? size_t(PyList_GET_SIZE(run.chunks[i].items)) : 0;
    }
    PyObject *exc = nullptr;
    int rc = 0;
    if (total > 0) {
      Py_DECREF(result);
      result = PyList_New(Py_ssize_t(total));
      rc = result == nullptr ? -1 : 0;
    }
    size_t k = 0;
    for (size_t i = 0; i < run.chunks.size(); i++) {
      ParallelRun::Chunk &c = run.chunks[i];
      if (c.items != nullptr && i <= f && rc == 0) {
        for (Py_ssize_t j = 0; j < PyList_GET_SIZE(c.items); j++) {
          PyList_SET_ITEM(result, Py_ssize_t(k++), Py_NewRef(PyList_GET_ITEM(c.items, j)));
        }
      }
      Py_XDECREF(c.items);
      if (i == f) {
        exc = c.exc;
        resume_at = c.resume_at;
      } else {
        Py_XDECREF(c.exc);
      }
    }
    if (rc < 0) {
      Py_XDECREF(exc);
      return nullptr;
    }
    if (exc != nullptr) { // fn raised: as it would have without threads
      Py_DECREF(result);
      PyErr_SetObject(reinterpret_cast<PyObject *>(Py_TYPE(exc)), exc);
      Py_DECREF(exc);
      return nullptr;
    }
    if (f == SIZE_MAX) {
      return result;
    }
  }
#else
  (void)threads;
#endif
  // One thread, from resume_at to the end.
  StreamObject *st = resume_at == 0 ? reinterpret_cast<StreamObject *>(
                                          Py_NewRef(reinterpret_cast<PyObject *>(whole)))
                                    : stream_over(whole, resume_at, whole->len);
  if (st == nullptr) {
    Py_DECREF(result);
    return nullptr;
  }
  for (;;) {
    PyObject *v = stream_next_locked(st);
    if (v == nullptr) {
      break;
    }
    if (fn != nullptr) {
      PyObject *r = PyObject_CallOneArg(fn, v);
      Py_DECREF(v);
      if (r == nullptr) {
        break;
      }
      v = r;
    }
    int rc = PyList_Append(result, v);
    Py_DECREF(v);
    if (rc < 0) {
      break;
    }
  }
  Py_DECREF(reinterpret_cast<PyObject *>(st));
  if (PyErr_Occurred()) {
    Py_DECREF(result);
    return nullptr;
  }
  return result;
}

// Removes the keyword `threads` from kwargs (into *threads: 0 when absent or
// None, else at least 1); returns the remaining keywords (new reference).
PyObject *take_threads(PyObject *kwargs, Py_ssize_t *threads, bool *given) {
  *threads = 0;
  *given = false;
  if (kwargs == nullptr) {
    return nullptr;
  }
  PyObject *rest = PyDict_Copy(kwargs);
  if (rest == nullptr) {
    return nullptr;
  }
  PyObject *t = PyDict_GetItemString(rest, "threads"); // borrowed
  if (t == nullptr) {
    return rest;
  }
  Py_INCREF(t);
  PyDict_DelItemString(rest, "threads");
  *given = true;
  if (t != Py_None) {
    Py_ssize_t n = PyNumber_AsSsize_t(t, PyExc_OverflowError);
    if (n == -1 && PyErr_Occurred()) {
      Py_DECREF(t);
      Py_DECREF(rest);
      return nullptr;
    }
    if (n < 0) {
      PyErr_SetString(PyExc_ValueError, "threads must be at least 0 (0: one per processor)");
      Py_DECREF(t);
      Py_DECREF(rest);
      return nullptr;
    }
    *threads = n == 0 ? Py_ssize_t(std::max(1u, std::thread::hardware_concurrency())) : n;
  } else {
    *threads = Py_ssize_t(std::max(1u, std::thread::hardware_concurrency()));
  }
  Py_DECREF(t);
  return rest;
}

// Removes the keyword `name` from the dict kwargs (may be nullptr); returns
// its value (new reference), or nullptr when absent or on error.
PyObject *pop_keyword(PyObject *kwargs, const char *name) {
  if (kwargs == nullptr) {
    return nullptr;
  }
  PyObject *v = PyDict_GetItemString(kwargs, name); // borrowed
  if (v == nullptr) {
    return nullptr;
  }
  Py_INCREF(v);
  if (PyDict_DelItemString(kwargs, name) < 0) {
    Py_DECREF(v);
    return nullptr;
  }
  return v;
}

// The stream for loads_many/parse_many/map_many; with threads, the list of
// all results instead. With fields (not for parse_many), the values at the
// fields' pointers come instead of the documents.
PyObject *stream_or_list(PyObject *args, PyObject *kwargs, bool lazy, PyObject *fn,
                         bool force_list, bool allow_fields) {
  Py_ssize_t threads;
  bool given;
  PyObject *rest = take_threads(kwargs, &threads, &given);
  if (rest == nullptr && PyErr_Occurred()) {
    return nullptr;
  }
  Fields *fields = nullptr;
  if (allow_fields) {
    PyObject *spec = pop_keyword(rest, "fields");
    PyObject *dflt = PyErr_Occurred() ? nullptr : pop_keyword(rest, "default");
    if (PyErr_Occurred()) {
      // reported below
    } else if (spec != nullptr && spec != Py_None) {
      if (lazy) {
        PyErr_SetString(PyExc_ValueError, "fields cannot be combined with lazy=True");
      } else {
        fields = compile_fields(spec, dflt);
      }
    } else if (dflt != nullptr) {
      PyErr_SetString(PyExc_TypeError, "default requires fields");
    }
    Py_XDECREF(spec);
    Py_XDECREF(dflt);
    if (PyErr_Occurred()) {
      Py_XDECREF(rest);
      return nullptr;
    }
  }
  PyObject *self = make_stream(args, rest, lazy);
  Py_XDECREF(rest);
  if (self == nullptr) {
    delete fields;
    return nullptr;
  }
  if (fields != nullptr) {
    reinterpret_cast<StreamObject *>(self)->fields = fields;
    reinterpret_cast<StreamObject *>(self)->owns_fields = true;
  }
  if (!given && !force_list) {
    return self;
  }
  StreamObject *whole = reinterpret_cast<StreamObject *>(self);
  using simdjson::stream_format;
  if (given && whole->format != stream_format::whitespace_delimited &&
      whole->format != stream_format::newline_delimited &&
      whole->format != stream_format::json_sequence) {
    Py_DECREF(self);
    PyErr_SetString(PyExc_ValueError,
                    "threads requires the format 'whitespace', 'lines' or 'json_seq'");
    return nullptr;
  }
  PyObject *r = run_stream(whole, fn, given ? size_t(threads) : 1);
  Py_DECREF(self);
  return r;
}

PyObject *loads_many(PyObject *, PyObject *args, PyObject *kwargs) {
  return stream_or_list(args, kwargs, false, nullptr, false, true);
}

PyObject *parse_many(PyObject *, PyObject *args, PyObject *kwargs) {
  return stream_or_list(args, kwargs, true, nullptr, false, false);
}

// map_many(fn, data, *, format, batch_size, threads, lazy=False, fields,
// default): [fn(doc) for doc in loads_many(data)] (parse_many with
// lazy=True), with fn called in the threads.
PyObject *map_many(PyObject *, PyObject *args, PyObject *kwargs) {
  if (PyTuple_GET_SIZE(args) < 1) {
    PyErr_SetString(PyExc_TypeError, "map_many() missing required argument 'fn'");
    return nullptr;
  }
  PyObject *fn = PyTuple_GET_ITEM(args, 0);
  if (!PyCallable_Check(fn)) {
    PyErr_SetString(PyExc_TypeError, "map_many() fn must be callable");
    return nullptr;
  }
  bool lazy = false;
  PyObject *rest = kwargs ? PyDict_Copy(kwargs) : nullptr;
  if (kwargs != nullptr && rest == nullptr) {
    return nullptr;
  }
  if (rest != nullptr) {
    PyObject *l = PyDict_GetItemString(rest, "lazy"); // borrowed
    if (l != nullptr) {
      int t = PyObject_IsTrue(l);
      if (t < 0 || PyDict_DelItemString(rest, "lazy") < 0) {
        Py_DECREF(rest);
        return nullptr;
      }
      lazy = t != 0;
    }
  }
  PyObject *tail = PyTuple_GetSlice(args, 1, PyTuple_GET_SIZE(args));
  if (tail == nullptr) {
    Py_XDECREF(rest);
    return nullptr;
  }
  PyObject *r = stream_or_list(tail, rest, lazy, fn, true, true);
  Py_DECREF(tail);
  Py_XDECREF(rest);
  return r;
}

// ---------------------------------------------------------------------------
// dumpb_many(records, /, default=None, option=None, *, threads=None): the
// records as NDJSON, each as dumpb writes it, followed by a newline. With
// threads (free-threaded Python), chunks of records are encoded at once.
// ---------------------------------------------------------------------------

// Encodes items [a, b) into e.out, one per line. Returns b, or the index of
// the record that failed, with the exception set.
size_t encode_lines(OrjsonEncoder &e, PyObject *const *items, size_t a, size_t b) {
  for (size_t i = a; i < b; i++) {
    size_t mark = e.out.len;
    e.depth = e.default_calls = 0;
#ifndef Py_GIL_DISABLED
    e.borrowed = true;
#endif
    int rc = e.encode(items[i]);
    if (rc < 0 && e.restart) { // Python code may run: again, without borrowing
      e.borrowed = e.restart = false;
      e.out.len = mark;
      e.depth = e.default_calls = 0;
      rc = e.encode(items[i]);
    }
    if (rc < 0) {
      return i;
    }
    e.out.push_back('\n');
  }
  return b;
}

#ifdef Py_GIL_DISABLED
struct DumpRun : PoolTask {
  PyObject *const *items;
  PyObject *dflt; // borrowed, or nullptr
  unsigned long opts;
  std::vector<size_t> bounds; // chunk i is items [bounds[i], bounds[i + 1])
  struct Chunk {
    std::unique_ptr<OrjsonEncoder> e; // its output
    PyObject *type = nullptr, *value = nullptr, *tb = nullptr; // its error
  };
  std::vector<Chunk> chunks;
  std::atomic<size_t> next{0};
  std::atomic<size_t> first_failure{SIZE_MAX};
  // Output of the chunks done so far, to size the buffers of the next ones
  // (growing a buffer copies it).
  std::atomic<size_t> done_bytes{0};
  std::atomic<size_t> done_records{0};

  // On an attached thread.
  void run_chunk(size_t i) {
    Chunk &c = chunks[i];
    bool failed;
    try {
      c.e.reset(new OrjsonEncoder()); // uses this thread's key cache
      c.e->default_fn = dflt;
      c.e->opts = opts;
      size_t records = bounds[i + 1] - bounds[i];
      size_t seen = done_records.load(std::memory_order_relaxed);
      if (seen > 0) {
        size_t per_record = done_bytes.load(std::memory_order_relaxed) / seen + 1;
        c.e->out.reserve(per_record * records + per_record * records / 8);
      }
      failed = encode_lines(*c.e, items, bounds[i], bounds[i + 1]) < bounds[i + 1];
      if (!failed) {
        done_bytes.fetch_add(c.e->out.len, std::memory_order_relaxed);
        done_records.fetch_add(records, std::memory_order_relaxed);
      }
    } catch (const std::bad_alloc &) {
      PyErr_NoMemory();
      failed = true;
    }
    if (failed) {
      PyErr_Fetch(&c.type, &c.value, &c.tb);
      size_t f = first_failure.load();
      while (i < f && !first_failure.compare_exchange_weak(f, i)) {
      }
    }
  }

  void work() override {
    for (;;) {
      size_t i = next.fetch_add(1);
      if (i >= chunks.size() || i > first_failure.load()) {
        return;
      }
      run_chunk(i);
      Py_BEGIN_ALLOW_THREADS
      Py_END_ALLOW_THREADS
    }
  }
};

// Copies the output of each chunk into place, on several threads too (one
// thread copies about 10 GB/s).
struct CopyRun : PoolTask {
  DumpRun *run;
  char *dst;
  std::vector<size_t> offsets;
  std::atomic<size_t> next{0};

  void work() override {
    for (;;) {
      size_t i = next.fetch_add(1);
      if (i >= run->chunks.size()) {
        return;
      }
      const OutBuf &o = run->chunks[i].e->out;
      memcpy(dst + offsets[i], o.buf, o.len);
    }
  }
};

// The records on several threads, or nullptr with an exception.
PyObject *dumpb_many_threads(PyObject *const *items, size_t n, PyObject *dflt,
                             unsigned long opts, size_t threads) {
  DumpRun run;
  run.items = items;
  run.dflt = dflt;
  run.opts = opts;
  size_t nchunks = std::min(threads * 8, n);
  for (size_t k = 0; k <= nchunks; k++) {
    run.bounds.push_back(n * k / nchunks);
  }
  run.chunks.resize(nchunks);
  run_on_pool(&run, std::min(threads, nchunks));
  size_t f = run.first_failure.load();
  PyObject *result = nullptr;
  if (f == SIZE_MAX) {
    CopyRun copy;
    copy.run = &run;
    size_t total = 0;
    for (auto &c : run.chunks) {
      copy.offsets.push_back(total);
      total += c.e->out.len;
    }
    result = PyBytes_FromStringAndSize(nullptr, Py_ssize_t(total));
    if (result != nullptr) {
      copy.dst = PyBytes_AS_STRING(result);
      run_on_pool(&copy, std::min(threads, nchunks));
    }
  }
  // The error of the first record that failed, as without threads.
  for (size_t i = 0; i < run.chunks.size(); i++) {
    DumpRun::Chunk &c = run.chunks[i];
    if (i == f) {
      PyErr_Restore(c.type, c.value, c.tb);
    } else {
      Py_XDECREF(c.type);
      Py_XDECREF(c.value);
      Py_XDECREF(c.tb);
    }
  }
  return result;
}
#endif

PyObject *dumpb_many(PyObject *, PyObject *args, PyObject *kwargs) {
  Py_ssize_t threads;
  bool given;
  PyObject *rest = take_threads(kwargs, &threads, &given);
  if (rest == nullptr && PyErr_Occurred()) {
    return nullptr;
  }
  static const char *kwlist[] = {"", "default", "option", nullptr};
  PyObject *records, *dflt = nullptr, *option = nullptr;
  int ok = PyArg_ParseTupleAndKeywords(args, rest, "O|OO:dumpb_many",
                                       const_cast<char **>(kwlist), &records, &dflt, &option);
  Py_XDECREF(rest);
  unsigned long opts;
  if (!ok || !orjson_options(option, &opts)) {
    return nullptr;
  }
  if (opts & OPT_INDENT_2) {
    PyErr_SetString(PyExc_ValueError, "dumpb_many writes one record per line: OPT_INDENT_2 is not supported");
    return nullptr;
  }
  // A snapshot of the records (a list could change while we encode it).
  PyObject *tuple = PySequence_Tuple(records);
  if (tuple == nullptr) {
    return nullptr;
  }
  PyObject *const *items = &PyTuple_GET_ITEM(tuple, 0);
  size_t n = size_t(PyTuple_GET_SIZE(tuple));
  PyObject *result = nullptr;
#ifdef Py_GIL_DISABLED
  if (given && threads > 1 && n > 1) {
    result = dumpb_many_threads(items, n, dflt, opts, size_t(threads));
    Py_DECREF(tuple);
    return result;
  }
#else
  (void)given;
#endif
  OrjsonEncoder e;
  e.default_fn = dflt;
  e.opts = opts;
  try {
    if (encode_lines(e, items, 0, n) == n) {
      result = e.out.take_bytes();
    }
  } catch (const std::bad_alloc &) {
    PyErr_NoMemory();
  }
  Py_DECREF(tuple);
  return result;
}

// ---------------------------------------------------------------------------
// Arrow: loads_arrow(data) and load_arrow_file(path) read NDJSON into Arrow
// record batches, dumpb_arrow(table) writes Arrow data as NDJSON. Arrow data
// goes in and out through the Arrow PyCapsule interface (__arrow_c_stream__,
// __arrow_c_schema__, __arrow_c_array__), so that pyarrow, polars, DuckDB...
// take it without a copy. simdjson_arrow does the work, without the GIL, on
// threads of its own.
// ---------------------------------------------------------------------------
PyTypeObject *ArrowStreamType = nullptr;

// The result of loads_arrow: record batches, handed over once.
struct ArrowStreamObject {
  PyObject_HEAD
  simdjson_arrow::field *schema;
  std::vector<ArrowArray> *batches; // nullptr once handed over
};

void arrow_stream_dealloc(PyObject *self) {
  ArrowStreamObject *s = reinterpret_cast<ArrowStreamObject *>(self);
  PyTypeObject *tp = Py_TYPE(self);
  if (s->batches != nullptr) {
    for (ArrowArray &b : *s->batches) {
      b.release(&b);
    }
    delete s->batches;
  }
  delete s->schema;
  PyObject_Free(self);
  Py_DECREF(tp);
}

void release_schema_capsule(PyObject *capsule) {
  auto *schema = static_cast<ArrowSchema *>(PyCapsule_GetPointer(capsule, "arrow_schema"));
  if (schema != nullptr && schema->release != nullptr) {
    schema->release(schema);
  }
  delete schema;
}

void release_stream_capsule(PyObject *capsule) {
  auto *stream =
      static_cast<ArrowArrayStream *>(PyCapsule_GetPointer(capsule, "arrow_array_stream"));
  if (stream != nullptr && stream->release != nullptr) {
    stream->release(stream);
  }
  delete stream;
}

// __arrow_c_schema__(): the schema of the records, as a capsule.
PyObject *arrow_stream_schema(PyObject *self, PyObject *) {
  ArrowStreamObject *s = reinterpret_cast<ArrowStreamObject *>(self);
  auto *schema = new (std::nothrow) ArrowSchema();
  if (schema == nullptr) {
    return PyErr_NoMemory();
  }
  simdjson_arrow::status st = simdjson_arrow::export_schema(*s->schema, schema);
  if (!st.ok()) {
    delete schema;
    PyErr_SetString(PyExc_ValueError, st.message.c_str());
    return nullptr;
  }
  PyObject *capsule = PyCapsule_New(schema, "arrow_schema", release_schema_capsule);
  if (capsule == nullptr) {
    schema->release(schema);
    delete schema;
  }
  return capsule;
}

// __arrow_c_stream__(requested_schema=None): the record batches, as a
// capsule. The batches are handed over: a second call raises ValueError. We
// ignore requested_schema, as the interface allows.
PyObject *arrow_stream_stream(PyObject *self, PyObject *args, PyObject *kwargs) {
  static const char *kwlist[] = {"requested_schema", nullptr};
  PyObject *requested = nullptr;
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|O:__arrow_c_stream__",
                                   const_cast<char **>(kwlist), &requested)) {
    return nullptr;
  }
  ArrowStreamObject *s = reinterpret_cast<ArrowStreamObject *>(self);
  auto *stream = new (std::nothrow) ArrowArrayStream();
  if (stream == nullptr) {
    return PyErr_NoMemory();
  }
  std::vector<ArrowArray> *batches;
  Py_BEGIN_CRITICAL_SECTION(self);
  batches = s->batches;
  s->batches = nullptr;
  Py_END_CRITICAL_SECTION();
  if (batches == nullptr) {
    delete stream;
    PyErr_SetString(PyExc_ValueError,
                    "the record batches of this stream were already taken: read the "
                    "JSON again to get them once more");
    return nullptr;
  }
  try {
    simdjson_arrow::make_stream(*s->schema, std::move(*batches), stream);
  } catch (const std::bad_alloc &) {
    for (ArrowArray &b : *batches) {
      b.release(&b);
    }
    delete batches;
    delete stream;
    return PyErr_NoMemory();
  }
  delete batches;
  PyObject *capsule = PyCapsule_New(stream, "arrow_array_stream", release_stream_capsule);
  if (capsule == nullptr) {
    stream->release(stream);
    delete stream;
  }
  return capsule;
}

// The number of records (rows), before the batches are taken.
PyObject *arrow_stream_num_rows(PyObject *self, void *) {
  ArrowStreamObject *s = reinterpret_cast<ArrowStreamObject *>(self);
  int64_t rows = 0;
  Py_BEGIN_CRITICAL_SECTION(self);
  if (s->batches != nullptr) {
    for (const ArrowArray &b : *s->batches) {
      rows += b.length;
    }
  } else {
    rows = -1;
  }
  Py_END_CRITICAL_SECTION();
  if (rows < 0) {
    Py_RETURN_NONE;
  }
  return PyLong_FromLongLong(rows);
}

PyMethodDef arrow_stream_methods[] = {
    {"__arrow_c_schema__", arrow_stream_schema, METH_NOARGS,
     "The schema of the records, as an Arrow PyCapsule."},
    {"__arrow_c_stream__",
     reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(arrow_stream_stream)),
     METH_VARARGS | METH_KEYWORDS,
     "The record batches, as an Arrow PyCapsule (once)."},
    {nullptr, nullptr, 0, nullptr},
};

PyGetSetDef arrow_stream_getset[] = {
    {"num_rows", arrow_stream_num_rows, nullptr,
     "The number of records, or None once the batches are taken.", nullptr},
    {nullptr, nullptr, nullptr, nullptr, nullptr},
};

PyType_Slot arrow_stream_slots[] = {
    {Py_tp_dealloc, reinterpret_cast<void *>(arrow_stream_dealloc)},
    {Py_tp_methods, arrow_stream_methods},
    {Py_tp_getset, arrow_stream_getset},
    {Py_tp_doc, const_cast<char *>(
                    "Record batches read by loads_arrow, for any library that takes the "
                    "Arrow PyCapsule interface: pyarrow.table(s), polars.DataFrame(s)...")},
    {0, nullptr},
};

PyType_Spec arrow_stream_spec = {
    "fastsimdjson.ArrowStream", sizeof(ArrowStreamObject), 0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION, arrow_stream_slots,
};

// Calls obj.<method>() and returns the pointer of the capsule it returns.
// *owner gets the capsule (a new reference), which owns the pointer.
void *capsule_of(PyObject *obj, const char *method, const char *name, PyObject **owner) {
  *owner = PyObject_CallMethod(obj, method, nullptr);
  if (*owner == nullptr) {
    return nullptr;
  }
  void *p = PyCapsule_GetPointer(*owner, name);
  if (p == nullptr) {
    Py_CLEAR(*owner);
  }
  return p;
}

// The schema given to loads_arrow: an object with __arrow_c_schema__ (a
// pyarrow.Schema...) or an "arrow_schema" capsule.
bool arrow_schema_argument(PyObject *arg, simdjson_arrow::field *out) {
  PyObject *owner = nullptr;
  const ArrowSchema *schema;
  if (PyCapsule_CheckExact(arg)) {
    schema = static_cast<const ArrowSchema *>(PyCapsule_GetPointer(arg, "arrow_schema"));
  } else if (PyObject_HasAttrString(arg, "__arrow_c_schema__")) {
    schema = static_cast<const ArrowSchema *>(
        capsule_of(arg, "__arrow_c_schema__", "arrow_schema", &owner));
  } else {
    PyErr_Format(PyExc_TypeError,
                 "schema must support the Arrow PyCapsule interface (__arrow_c_schema__), "
                 "like a pyarrow.Schema, not %.200s",
                 Py_TYPE(arg)->tp_name);
    return false;
  }
  if (schema == nullptr) {
    return false;
  }
  simdjson_arrow::status st;
  try {
    st = simdjson_arrow::import_schema(schema, out);
  } catch (const std::bad_alloc &) {
    st.message = "out of memory";
  }
  Py_XDECREF(owner);
  if (!st.ok()) {
    PyErr_Format(PyExc_ValueError, "unsupported schema: %s", st.message.c_str());
    return false;
  }
  return true;
}

struct ArrowReadArguments {
  simdjson_arrow::field schema;
  bool infer = true;
  simdjson_arrow::ndjson_options options;
};

// Parses the arguments of loads_arrow and load_arrow_file, but the first.
// *first gets the first argument (borrowed).
bool arrow_read_arguments(const char *function, PyObject *args, PyObject *kwargs,
                          PyObject **first, ArrowReadArguments *out) {
  Py_ssize_t threads;
  bool given;
  PyObject *rest = take_threads(kwargs, &threads, &given);
  if (rest == nullptr && PyErr_Occurred()) {
    return false;
  }
  static const char *kwlist[] = {"",           "schema", "block_size", "error_on_unknown_fields",
                                 "validate_skipped", nullptr};
  PyObject *schema = Py_None;
  Py_ssize_t block_size = Py_ssize_t(out->options.block_size);
  int error_on_unknown_fields = 0, validate_skipped = 1;
  std::string format = std::string("O|O$npp:") + function;
  int ok = PyArg_ParseTupleAndKeywords(args, rest, format.c_str(),
                                       const_cast<char **>(kwlist), first, &schema,
                                       &block_size, &error_on_unknown_fields,
                                       &validate_skipped);
  Py_XDECREF(rest);
  if (!ok) {
    return false;
  }
  if (block_size <= 0) {
    PyErr_SetString(PyExc_ValueError, "block_size must be positive");
    return false;
  }
  out->options.block_size = size_t(block_size);
  out->options.threads = given ? size_t(threads) : 1;
  out->options.error_on_unknown_fields = error_on_unknown_fields != 0;
  out->options.validate_skipped = validate_skipped != 0;
  if (schema != Py_None) {
    out->infer = false;
    return arrow_schema_argument(schema, &out->schema);
  }
  return true;
}

// Reads json[0, length) (without padding) into a new ArrowStream, without the
// GIL.
PyObject *read_arrow(const char *json, size_t length, ArrowReadArguments &a) {
  auto *batches = new (std::nothrow) std::vector<ArrowArray>();
  auto *schema = new (std::nothrow) simdjson_arrow::field(std::move(a.schema));
  if (batches == nullptr || schema == nullptr) {
    delete batches;
    delete schema;
    return PyErr_NoMemory();
  }
  simdjson_arrow::status st;
  Py_BEGIN_ALLOW_THREADS
  try {
    st = a.infer ? simdjson_arrow::read_ndjson(json, length, schema, batches, a.options)
                 : simdjson_arrow::read_ndjson(*schema, json, length, batches, a.options);
  } catch (const std::bad_alloc &) {
    st.message = "out of memory";
  }
  Py_END_ALLOW_THREADS
  if (!st.ok()) {
    for (ArrowArray &b : *batches) {
      b.release(&b);
    }
    delete batches;
    delete schema;
    if (st.message == "out of memory") {
      return PyErr_NoMemory();
    }
    PyErr_SetString(PyExc_ValueError, st.message.c_str());
    return nullptr;
  }
  ArrowStreamObject *s = PyObject_New(ArrowStreamObject, ArrowStreamType);
  if (s == nullptr) {
    for (ArrowArray &b : *batches) {
      b.release(&b);
    }
    delete batches;
    delete schema;
    return nullptr;
  }
  s->schema = schema;
  s->batches = batches;
  return reinterpret_cast<PyObject *>(s);
}

// Reads the bytes of data (any contiguous buffer, or a str) into an
// ArrowStream. The buffer is held (a bytearray cannot be resized) while we
// read it without the GIL.
PyObject *read_arrow_input(PyObject *data, ArrowReadArguments &a) {
  if (PyUnicode_Check(data)) {
    Py_ssize_t len;
    const char *s = PyUnicode_AsUTF8AndSize(data, &len);
    if (s == nullptr) {
      return nullptr;
    }
    return read_arrow(s, size_t(len), a);
  }
  if (!PyObject_CheckBuffer(data)) {
    PyErr_Format(PyExc_TypeError,
                 "Input must be bytes, bytearray, memoryview, or str, not %.200s",
                 Py_TYPE(data)->tp_name);
    return nullptr;
  }
  Py_buffer view;
  if (PyObject_GetBuffer(data, &view, PyBUF_SIMPLE) < 0) {
    return nullptr;
  }
  PyObject *r = read_arrow(static_cast<const char *>(view.buf), size_t(view.len), a);
  PyBuffer_Release(&view);
  return r;
}

PyObject *loads_arrow(PyObject *, PyObject *args, PyObject *kwargs) {
  ArrowReadArguments a;
  PyObject *data;
  if (!arrow_read_arguments("loads_arrow", args, kwargs, &data, &a)) {
    return nullptr;
  }
  return read_arrow_input(data, a);
}

PyObject *mmap_mmap = nullptr; // mmap.mmap
PyObject *mmap_ACCESS_READ = nullptr;

// The file at path, mapped in memory (an mmap object), or b"" if it is empty:
// the threads that read it load its pages, instead of one thread copying it.
PyObject *map_file(PyObject *path) {
  if (mmap_mmap == nullptr) {
    PyObject *module = PyImport_ImportModule("mmap");
    if (module == nullptr) {
      return nullptr;
    }
    PyObject *access = PyObject_GetAttrString(module, "ACCESS_READ");
    PyObject *mapper = PyObject_GetAttrString(module, "mmap");
    Py_DECREF(module);
    if (access == nullptr || mapper == nullptr) {
      Py_XDECREF(access);
      Py_XDECREF(mapper);
      return nullptr;
    }
    mmap_ACCESS_READ = access;
    mmap_mmap = mapper;
  }
  PyObject *f = PyObject_CallFunction(io_open, "Os", path, "rb");
  if (f == nullptr) {
    return nullptr;
  }
  PyObject *mapped = nullptr;
  PyObject *fileno = PyObject_CallMethod(f, "fileno", nullptr);
  PyObject *size = PyObject_CallMethod(f, "seek", "ii", 0, 2);
  if (fileno != nullptr && size != nullptr) {
    if (PyObject_IsTrue(size)) {
      PyObject *args = Py_BuildValue("(Oi)", fileno, 0);
      PyObject *kwargs = Py_BuildValue("{sO}", "access", mmap_ACCESS_READ);
      if (args != nullptr && kwargs != nullptr) {
        mapped = PyObject_Call(mmap_mmap, args, kwargs);
      }
      Py_XDECREF(args);
      Py_XDECREF(kwargs);
    } else {
      mapped = PyBytes_FromStringAndSize(nullptr, 0); // mmap cannot map an empty file
    }
  }
  Py_XDECREF(fileno);
  Py_XDECREF(size);
  PyObject *r = PyObject_CallMethod(f, "close", nullptr); // the mapping stays valid
  Py_DECREF(f);
  if (r == nullptr) {
    Py_XDECREF(mapped);
    return nullptr;
  }
  Py_DECREF(r);
  return mapped;
}

PyObject *load_arrow_file(PyObject *, PyObject *args, PyObject *kwargs) {
  ArrowReadArguments a;
  PyObject *path;
  if (!arrow_read_arguments("load_arrow_file", args, kwargs, &path, &a)) {
    return nullptr;
  }
  PyObject *mapped = map_file(path);
  if (mapped == nullptr) {
    return nullptr;
  }
  PyObject *r = read_arrow_input(mapped, a);
  if (!PyBytes_Check(mapped)) {
    PyObject *type, *value, *tb;
    PyErr_Fetch(&type, &value, &tb); // the error of the read, if any
    PyObject *c = PyObject_CallMethod(mapped, "close", nullptr);
    if (c == nullptr) {
      Py_CLEAR(r);
      Py_XDECREF(type);
      Py_XDECREF(value);
      Py_XDECREF(tb);
    } else {
      Py_DECREF(c);
      PyErr_Restore(type, value, tb);
    }
  }
  Py_DECREF(mapped);
  return r;
}

// The output of dumpb_arrow: a bytes object that the writer grows (taking
// the GIL, which it does not hold otherwise), handed over without a copy.
struct BytesAllocator {
  PyObject *bytes = nullptr;
  size_t capacity = 0;

  static char *reallocate(void *context, char *, size_t, size_t size) {
    BytesAllocator *self = static_cast<BytesAllocator *>(context);
    if (size > size_t(PY_SSIZE_T_MAX)) {
      return nullptr;
    }
    PyGILState_STATE gil = PyGILState_Ensure();
    if (self->bytes == nullptr) {
      self->bytes = PyBytes_FromStringAndSize(nullptr, Py_ssize_t(size));
    } else if (_PyBytes_Resize(&self->bytes, Py_ssize_t(size)) < 0) {
      self->bytes = nullptr;
    }
    char *p = nullptr;
    if (self->bytes == nullptr) {
      PyErr_Clear(); // the writer reports that it is out of memory
      self->capacity = 0;
    } else {
      p = PyBytes_AS_STRING(self->bytes);
      self->capacity = size;
    }
    PyGILState_Release(gil);
    return p;
  }
  static void deallocate(void *context, char *) {
    BytesAllocator *self = static_cast<BytesAllocator *>(context);
    PyGILState_STATE gil = PyGILState_Ensure();
    Py_CLEAR(self->bytes);
    PyGILState_Release(gil);
  }
  // The bytes object, cut to its first size bytes (after release).
  PyObject *take(size_t size) {
    if (size >= capacity / 2) {
      // At least half full: keep the allocation, as OutBuf does.
      Py_SET_SIZE(bytes, Py_ssize_t(size));
      PyBytes_AS_STRING(bytes)[size] = '\0';
    } else if (_PyBytes_Resize(&bytes, Py_ssize_t(size)) < 0) {
      bytes = nullptr;
      return nullptr;
    }
    PyObject *r = bytes;
    bytes = nullptr;
    return r;
  }
  ~BytesAllocator() { Py_XDECREF(bytes); }
};

// dumpb_arrow(data, *, write_nulls=True, threads=None): data (an object with
// __arrow_c_stream__ or __arrow_c_array__) as NDJSON bytes.
PyObject *dumpb_arrow(PyObject *, PyObject *args, PyObject *kwargs) {
  Py_ssize_t threads;
  bool given;
  PyObject *rest = take_threads(kwargs, &threads, &given);
  if (rest == nullptr && PyErr_Occurred()) {
    return nullptr;
  }
  static const char *kwlist[] = {"", "write_nulls", nullptr};
  PyObject *data;
  int write_nulls = 1;
  int ok = PyArg_ParseTupleAndKeywords(args, rest, "O|$p:dumpb_arrow",
                                       const_cast<char **>(kwlist), &data, &write_nulls);
  Py_XDECREF(rest);
  if (!ok) {
    return nullptr;
  }
  simdjson_arrow::ndjson_write_options options;
  options.write_nulls = write_nulls != 0;
  options.threads = given ? size_t(threads) : 1;
  // A stream we take over, or a schema and a batch that their capsules own.
  ArrowArrayStream stream{};
  const ArrowSchema *schema = nullptr;
  const ArrowArray *batch = nullptr;
  PyObject *owner = nullptr;
  if (PyObject_HasAttrString(data, "__arrow_c_stream__")) {
    auto *p = static_cast<ArrowArrayStream *>(
        capsule_of(data, "__arrow_c_stream__", "arrow_array_stream", &owner));
    if (p == nullptr) {
      return nullptr;
    }
    if (p->release == nullptr) {
      Py_DECREF(owner);
      PyErr_SetString(PyExc_ValueError, "the Arrow stream was released");
      return nullptr;
    }
    stream = *p; // we move the stream out of its capsule
    p->release = nullptr;
    Py_CLEAR(owner);
  } else if (PyObject_HasAttrString(data, "__arrow_c_array__")) {
    owner = PyObject_CallMethod(data, "__arrow_c_array__", nullptr);
    if (owner == nullptr) {
      return nullptr;
    }
    if (!PyTuple_Check(owner) || PyTuple_GET_SIZE(owner) != 2) {
      Py_DECREF(owner);
      PyErr_SetString(PyExc_TypeError, "__arrow_c_array__ must return a pair of capsules");
      return nullptr;
    }
    schema = static_cast<const ArrowSchema *>(
        PyCapsule_GetPointer(PyTuple_GET_ITEM(owner, 0), "arrow_schema"));
    batch = static_cast<const ArrowArray *>(
        schema ? PyCapsule_GetPointer(PyTuple_GET_ITEM(owner, 1), "arrow_array") : nullptr);
    if (batch == nullptr) {
      Py_DECREF(owner);
      return nullptr;
    }
  } else {
    PyErr_Format(PyExc_TypeError,
                 "dumpb_arrow takes Arrow data (an object with __arrow_c_stream__ or "
                 "__arrow_c_array__: a pyarrow.Table, a polars.DataFrame...), not %.200s",
                 Py_TYPE(data)->tp_name);
    return nullptr;
  }
  BytesAllocator bytes;
  simdjson_arrow::ndjson_allocator allocator{BytesAllocator::reallocate,
                                             BytesAllocator::deallocate, &bytes};
  simdjson_arrow::status st;
  size_t size = 0;
  bool released = false;
  Py_BEGIN_ALLOW_THREADS
  try {
    simdjson_arrow::ndjson_writer writer(options, allocator);
    st = batch != nullptr ? writer.write(schema, batch) : writer.write(&stream);
    if (st.ok()) {
      writer.release(&size);
      released = true;
    }
  } catch (const std::bad_alloc &) {
    st.message = "out of memory";
  }
  Py_END_ALLOW_THREADS
  Py_XDECREF(owner);
  if (!st.ok() || !released) {
    if (st.message == "out of memory") {
      return PyErr_NoMemory();
    }
    PyErr_SetString(PyExc_ValueError, st.message.c_str());
    return nullptr;
  }
  return bytes.take(size);
}

// ---------------------------------------------------------------------------
// MessagePack: json_to_msgpack(data) converts JSON (a document, or NDJSON) to
// MessagePack, msgpack_to_json(data) converts MessagePack (a value, or a
// sequence of values) to JSON. simdjson_msgpack does the work, without the
// GIL, on several threads if asked, into a bytes object (BytesAllocator: no
// final copy). No Python object is made from the data.
// ---------------------------------------------------------------------------

// The bytes of data (any contiguous buffer, or a str with text), read without
// the GIL. bytes and str cannot change, so we read them in place. Any other
// buffer (bytearray, memoryview, array...) could be written by another thread
// meanwhile: we copy it first, with the padding simdjson reads past the end.
struct InputBytes {
  simdjson::padded_string copy;
  const char *data = nullptr;
  size_t size = 0;
  bool padded = false;  // data is followed by SIMDJSON_PADDING bytes we own

  bool get(PyObject *o, bool text) {
    if (text && PyUnicode_Check(o)) {
      Py_ssize_t len;
      data = PyUnicode_AsUTF8AndSize(o, &len);
      size = size_t(len);
      return data != nullptr;
    }
    if (PyBytes_Check(o)) {
      data = PyBytes_AS_STRING(o);
      size = size_t(PyBytes_GET_SIZE(o));
      return true;
    }
    if (!PyObject_CheckBuffer(o)) {
      PyErr_Format(PyExc_TypeError,
                   text ? "Input must be bytes, bytearray, memoryview, or str, not %.200s"
                        : "Input must be bytes, bytearray or memoryview, not %.200s",
                   Py_TYPE(o)->tp_name);
      return false;
    }
    Py_buffer view;
    if (PyObject_GetBuffer(o, &view, PyBUF_SIMPLE) < 0) {
      return false;
    }
    copy = simdjson::padded_string(static_cast<const char *>(view.buf), size_t(view.len));
    PyBuffer_Release(&view);
    if (copy.data() == nullptr) {
      PyErr_NoMemory();
      return false;
    }
    data = copy.data();
    size = copy.size();
    padded = true;
    return true;
  }
};

// The result of a writer: the bytes object, or ValueError (MemoryError).
PyObject *msgpack_result(const simdjson_msgpack::status &st, bool released, BytesAllocator &bytes,
                         size_t size) {
  if (!st.ok() || !released) {
    if (st.message == "out of memory") {
      return PyErr_NoMemory();
    }
    PyErr_SetString(PyExc_ValueError, st.message.c_str());
    return nullptr;
  }
  return bytes.take(size);
}

// json_to_msgpack(data, *, ndjson=False, threads=None)
PyObject *json_to_msgpack(PyObject *, PyObject *args, PyObject *kwargs) {
  Py_ssize_t threads;
  bool given;
  PyObject *rest = take_threads(kwargs, &threads, &given);
  if (rest == nullptr && PyErr_Occurred()) {
    return nullptr;
  }
  static const char *kwlist[] = {"", "ndjson", "block_size", nullptr};
  PyObject *data;
  int ndjson = 0;
  simdjson_msgpack::msgpack_options options;
  Py_ssize_t block_size = Py_ssize_t(options.block_size);
  int ok = PyArg_ParseTupleAndKeywords(args, rest, "O|$pn:json_to_msgpack",
                                       const_cast<char **>(kwlist), &data, &ndjson, &block_size);
  Py_XDECREF(rest);
  if (!ok) {
    return nullptr;
  }
  if (block_size <= 0) {
    PyErr_SetString(PyExc_ValueError, "block_size must be positive");
    return nullptr;
  }
  options.threads = given ? size_t(threads) : 1;
  options.block_size = size_t(block_size);
  InputBytes in;
  if (!in.get(data, true)) {
    return nullptr;
  }
  BytesAllocator bytes;
  const simdjson_msgpack::allocator allocator{BytesAllocator::reallocate,
                                              BytesAllocator::deallocate, &bytes};
  simdjson_msgpack::status st;
  size_t size = 0;
  bool released = false;
  Py_BEGIN_ALLOW_THREADS
  try {
    simdjson_msgpack::msgpack_writer writer(options, allocator);
    if (!in.padded && needs_unpadded(in.data, in.size)) {
      // simdjson reads SIMDJSON_PADDING bytes past the end: the writer copies.
      st = ndjson ? writer.write_ndjson(in.data, in.size) : writer.write_json(in.data, in.size);
    } else {
      // The padding stays on the page of the last byte: we read in place.
      simdjson::padded_string_view json(in.data, in.size, in.size + simdjson::SIMDJSON_PADDING);
      st = ndjson ? writer.write_ndjson(json) : writer.write_json(json);
    }
    if (st.ok()) {
      writer.release(&size);
      released = true;
    }
  } catch (const std::bad_alloc &) {
    st.message = "out of memory";
  }
  Py_END_ALLOW_THREADS
  return msgpack_result(st, released, bytes, size);
}

// msgpack_to_json(data, *, stream=False, validate_utf8=True,
// binary_as_base64=True, ext_as_null=False, threads=None)
PyObject *msgpack_to_json(PyObject *, PyObject *args, PyObject *kwargs) {
  Py_ssize_t threads;
  bool given;
  PyObject *rest = take_threads(kwargs, &threads, &given);
  if (rest == nullptr && PyErr_Occurred()) {
    return nullptr;
  }
  static const char *kwlist[] = {"",           "stream",      "validate_utf8", "binary_as_base64",
                                 "ext_as_null", "block_size", nullptr};
  PyObject *data;
  int stream = 0, validate_utf8 = 1, binary_as_base64 = 1, ext_as_null = 0;
  simdjson_msgpack::json_options options;
  Py_ssize_t block_size = Py_ssize_t(options.block_size);
  int ok = PyArg_ParseTupleAndKeywords(args, rest, "O|$ppppn:msgpack_to_json",
                                       const_cast<char **>(kwlist), &data, &stream,
                                       &validate_utf8, &binary_as_base64, &ext_as_null,
                                       &block_size);
  Py_XDECREF(rest);
  if (!ok) {
    return nullptr;
  }
  if (block_size <= 0) {
    PyErr_SetString(PyExc_ValueError, "block_size must be positive");
    return nullptr;
  }
  options.validate_utf8 = validate_utf8 != 0;
  options.binary_as_base64 = binary_as_base64 != 0;
  options.ext = ext_as_null ? simdjson_msgpack::ext_policy::null : simdjson_msgpack::ext_policy::error;
  options.threads = given ? size_t(threads) : 1;
  options.block_size = size_t(block_size);
  InputBytes in;
  if (!in.get(data, false)) {
    return nullptr;
  }
  BytesAllocator bytes;
  const simdjson_msgpack::allocator allocator{BytesAllocator::reallocate,
                                              BytesAllocator::deallocate, &bytes};
  simdjson_msgpack::status st;
  size_t size = 0;
  bool released = false;
  Py_BEGIN_ALLOW_THREADS
  try {
    simdjson_msgpack::json_writer writer(options, allocator);
    st = stream ? writer.write_msgpack_stream(in.data, in.size)
                : writer.write_msgpack(in.data, in.size);
    if (st.ok()) {
      writer.release(&size);
      released = true;
    }
  } catch (const std::bad_alloc &) {
    st.message = "out of memory";
  }
  Py_END_ALLOW_THREADS
  return msgpack_result(st, released, bytes, size);
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
  release_thread_state();
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
     "Iterate over the JSON documents of data (NDJSON, JSON Lines, ...), like loads; "
     "with threads=N, the list of the documents, built by N threads."},
    {"parse_many", reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(parse_many)),
     METH_VARARGS | METH_KEYWORDS,
     "Iterate over the JSON documents of data lazily, like parse."},
#ifdef Py_GIL_DISABLED
    {"_stop_pool", stop_pool, METH_NOARGS, "Stop the worker threads (at exit)."},
#endif
    {"dumpb_many", reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(dumpb_many)),
     METH_VARARGS | METH_KEYWORDS,
     "dumpb_many(records, /, default=None, option=None, *, threads=None)\n--\n\n"
     "The records as NDJSON bytes: each as dumpb writes it, then a newline.\n"
     "With threads (free-threaded Python), the records are encoded on several threads."},
    {"loads_arrow", reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(loads_arrow)),
     METH_VARARGS | METH_KEYWORDS,
     "loads_arrow(data, /, schema=None, *, block_size=1048576, error_on_unknown_fields=False, "
     "validate_skipped=True, threads=None)\n--\n\n"
     "Read NDJSON into Arrow record batches (an ArrowStream), given a schema or inferring it."},
    {"load_arrow_file",
     reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(load_arrow_file)),
     METH_VARARGS | METH_KEYWORDS,
     "load_arrow_file(path, /, schema=None, *, block_size=1048576, "
     "error_on_unknown_fields=False, validate_skipped=True, threads=None)\n--\n\n"
     "Read the NDJSON file at path into Arrow record batches, like loads_arrow."},
    {"dumpb_arrow", reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(dumpb_arrow)),
     METH_VARARGS | METH_KEYWORDS,
     "dumpb_arrow(data, /, *, write_nulls=True, threads=None)\n--\n\n"
     "Arrow data (a pyarrow.Table, a polars.DataFrame...) as NDJSON bytes: one object per row."},
    {"json_to_msgpack",
     reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(json_to_msgpack)),
     METH_VARARGS | METH_KEYWORDS,
     "json_to_msgpack(data, /, *, ndjson=False, block_size=1048576, threads=None)\n--\n\n"
     "JSON (bytes, bytearray, memoryview or str) as MessagePack bytes, in the most compact "
     "encoding. With ndjson, each document of the input becomes one MessagePack value."},
    {"msgpack_to_json",
     reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(msgpack_to_json)),
     METH_VARARGS | METH_KEYWORDS,
     "msgpack_to_json(data, /, *, stream=False, validate_utf8=True, binary_as_base64=True, "
     "ext_as_null=False, block_size=1048576, threads=None)\n--\n\n"
     "MessagePack (a bytes-like object) as compact JSON bytes. With stream, each of the "
     "values of the input becomes one line of NDJSON."},
    {"map_many", reinterpret_cast<PyCFunction>(reinterpret_cast<void (*)(void)>(map_many)),
     METH_VARARGS | METH_KEYWORDS,
     "map_many(fn, data, *, format, batch_size, threads, lazy, fields, default): the list of "
     "fn(doc) for the "
     "documents of data, with fn called in the threads."},
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
#ifdef Py_GIL_DISABLED
  {
    PyObject *atexit = PyImport_ImportModule("atexit");
    PyObject *stop = atexit ? PyObject_GetAttrString(module, "_stop_pool") : nullptr;
    PyObject *r = stop ? PyObject_CallMethod(atexit, "register", "O", stop) : nullptr;
    Py_XDECREF(r);
    Py_XDECREF(stop);
    Py_XDECREF(atexit);
    if (r == nullptr) {
      return -1;
    }
#ifndef _WIN32
    static bool atfork_done = false;
    if (!atfork_done) {
      pthread_atfork(nullptr, nullptr, pool_after_fork);
      atfork_done = true;
    }
#endif
  }
#endif
#if FSJ_AVX512
  has_avx512 = detect_avx512();
#endif
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
               {&StreamType, &stream_spec},     {&ArrowStreamType, &arrow_stream_spec}};
  for (auto &t : types) {
    if (*t.type == nullptr) {
      *t.type = reinterpret_cast<PyTypeObject *>(PyType_FromSpec(t.spec));
      if (*t.type == nullptr) {
        return -1;
      }
#if defined(Py_GIL_DISABLED) && PY_VERSION_HEX >= 0x030E0000
      // Each instance holds a reference to its heap type, released in its
      // dealloc with Py_DECREF. CPython takes that reference with a
      // per-thread count, but without the GIL, Py_DECREF is an atomic
      // operation on the type, which all the threads making views contend
      // for (views from 64 threads ran about 100 times slower than from
      // one). Immortal types are not counted. The types are kept for the
      // life of the process anyway (no subinterpreters).
      _Py_SetImmortal(reinterpret_cast<PyObject *>(*t.type));
#endif
    }
  }
  // isinstance(x, Mapping) / isinstance(x, Sequence) hold for the views.
  if (register_abc("Mapping", ObjectType) < 0 || register_abc("Sequence", ArrayType) < 0 ||
      PyModule_AddObjectRef(module, "Object", reinterpret_cast<PyObject *>(ObjectType)) < 0 ||
      PyModule_AddObjectRef(module, "Array", reinterpret_cast<PyObject *>(ArrayType)) < 0 ||
      PyModule_AddObjectRef(module, "ArrowStream",
                            reinterpret_cast<PyObject *>(ArrowStreamType)) < 0) {
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
