// Typed decoding: loads(data, type=T) builds instances of T (dataclasses,
// TypedDicts, NamedTuples, enums, containers, unions, literals and scalars)
// straight from the simdjson tape, checking the types on the way.
//
// A type is described once, in Python (typing and dataclasses do the work),
// as a list of nodes; the nodes are compiled into a TypePlan cached per type.
// The same decoder runs on the tape and, for the documents that only json
// accepts (NaN, numbers out of range), on the Python objects json made.
//
// Included in the anonymous namespace of fastsimdjson.cpp.

PyObject *ValidationError = nullptr;
PyObject *typed_describe = nullptr; // the Python function below
PyObject *typed_cache = nullptr;    // type -> capsule holding its TypePlan
PyObject *typed_post_init = nullptr;
PyObject *typed_empty_tuple = nullptr;

const char typed_describe_source[] = R"PY(
import collections.abc as abc
import dataclasses
import enum
import typing
import types

NULL, BOOL, INT, FLOAT, STR, ARRAY, OBJECT = (1 << k for k in range(7))
ANY = 127
UNIONS = (typing.Union, getattr(types, "UnionType", typing.Union))
LISTS = (list, abc.Sequence, abc.MutableSequence, abc.Collection, abc.Iterable)
SETS = (set, abc.Set, abc.MutableSet)
DICTS = (dict, abc.Mapping, abc.MutableMapping)
WRAPPERS = tuple(getattr(typing, n) for n in ("Annotated", "Required", "NotRequired",
                                              "ReadOnly", "Final") if hasattr(typing, n))
KIND_NAMES = ("null", "bool", "int", "float", "str", "array", "object")


def strip(t):
    while True:
        if typing.get_origin(t) in WRAPPERS:
            t = typing.get_args(t)[0]
        elif hasattr(t, "__supertype__"):  # NewType
            t = t.__supertype__
        elif type(t).__name__ == "TypeAliasType":  # type X = ...
            t = t.__value__
        else:
            return t


def value_mask(v):
    if v is None:
        return NULL
    if isinstance(v, bool):
        return BOOL
    if isinstance(v, int):
        return INT
    if isinstance(v, str):
        return STR
    raise TypeError(f"Literal and Enum values must be str, int, bool or None, not {v!r}")


def base_of(t):
    return typing.get_origin(t) or t


def mask(t):
    t = strip(t)
    if t is typing.Any or t is object:
        return ANY
    if t is None or t is type(None):
        return NULL
    if t is bool:
        return BOOL
    if t is int:
        return INT
    if t is float:
        return INT | FLOAT
    if t is str:
        return STR
    origin = typing.get_origin(t)
    if origin in UNIONS:
        m = 0
        for a in typing.get_args(t):
            m |= mask(a)
        return m
    if origin is typing.Literal:
        m = 0
        for v in typing.get_args(t):
            m |= value_mask(v)
        return m
    base = base_of(t)
    if base in LISTS or base in SETS or base is frozenset or base is tuple:
        return ARRAY
    if base in DICTS:
        return OBJECT
    if isinstance(t, type):
        if issubclass(t, enum.Enum):
            m = 0
            for member in t:
                m |= value_mask(member.value)
            return m
        if dataclasses.is_dataclass(t) or typing.is_typeddict(t):
            return OBJECT
        if issubclass(t, tuple) and hasattr(t, "_fields"):
            return ARRAY
    return 0


def describe(root):
    nodes = []
    memo = {}

    def visit(t):
        try:
            return memo[t]
        except (KeyError, TypeError):
            pass
        i = len(nodes)
        nodes.append(None)
        try:
            memo[t] = i
        except TypeError:
            pass
        nodes[i] = node(t)
        return i

    def node(t):
        if isinstance(t, (str, typing.ForwardRef)):
            raise TypeError(f"Unresolved forward reference {t!r}")
        t = strip(t)
        m = mask(t)
        if m == ANY:
            return ("any", m)
        for scalar, name in ((None, "none"), (type(None), "none"), (bool, "bool"),
                             (int, "int"), (float, "float"), (str, "str")):
            if t is scalar:
                return (name, m)
        origin, args = typing.get_origin(t), typing.get_args(t)
        if origin in UNIONS:
            members = [a for a in args if strip(a) not in (None, type(None))]
            if len(members) == 1:
                return ("optional", m, visit(members[0]))
            table = [-1] * 7
            float_owner = [False] * 7
            for a in args:
                i = visit(a)
                am = mask(a)
                is_float = strip(a) is float
                for k in range(7):
                    if not am >> k & 1:
                        continue
                    if table[k] != -1:
                        # An integer goes to int rather than to float.
                        if k == 2 and is_float:
                            continue
                        if not (k == 2 and float_owner[k]):
                            raise TypeError(f"Type unions may not contain more than one "
                                            f"{KIND_NAMES[k]} type - {t!r} is not supported")
                    table[k] = i
                    float_owner[k] = is_float
            return ("union", m, tuple(table))
        if origin is typing.Literal:
            return ("literal", m, frozenset(args))
        base = base_of(t)
        if base in LISTS:
            return ("list", m, visit(args[0] if args else typing.Any))
        if base in SETS:
            return ("set", m, visit(args[0] if args else typing.Any))
        if base is frozenset:
            return ("frozenset", m, visit(args[0] if args else typing.Any))
        if base is tuple:
            if not args:
                return ("vartuple", m, visit(typing.Any))
            if len(args) == 2 and args[1] is Ellipsis:
                return ("vartuple", m, visit(args[0]))
            if args == ((),):
                return ("tuple", m, ())
            return ("tuple", m, tuple(visit(a) for a in args))
        if base in DICTS:
            key = strip(args[0]) if args else str
            if key not in (str, typing.Any, object):
                raise TypeError(f"Only str keys are supported in dicts - {t!r} is not supported")
            return ("dict", m, visit(args[1] if args else typing.Any))
        if isinstance(t, type) and m:
            if issubclass(t, enum.Enum):
                return ("enum", m, t, {member.value: member for member in t})
            if dataclasses.is_dataclass(t):
                hints = typing.get_type_hints(t)
                for name, h in hints.items():
                    if isinstance(h, dataclasses.InitVar):
                        raise TypeError(f"InitVar fields are not supported - {t!r}")
                fields = []
                for f in dataclasses.fields(t):
                    if f.default is not dataclasses.MISSING:
                        kind, default = 1, f.default
                    elif f.default_factory is not dataclasses.MISSING:
                        kind, default = 2, f.default_factory
                    else:
                        kind, default = 0, None
                    child = visit(hints[f.name]) if f.init else -1
                    fields.append((f.name, child, kind, default))
                return ("dataclass", m, t, tuple(fields), hasattr(t, "__post_init__"))
            if typing.is_typeddict(t):
                hints = typing.get_type_hints(t)
                required = t.__required_keys__
                return ("typeddict", m, t,
                        tuple((k, visit(v), 0 if k in required else 3, None)
                              for k, v in hints.items()))
            if issubclass(t, tuple) and hasattr(t, "_fields"):
                hints = typing.get_type_hints(t) if getattr(t, "__annotations__", None) else {}
                defaults = tuple(t._field_defaults[f] for f in t._fields
                                 if f in t._field_defaults)
                return ("namedtuple", m, t,
                        tuple(visit(hints.get(f, typing.Any)) for f in t._fields), defaults)
        raise TypeError(f"Type {t!r} is not supported")

    root_index = visit(root)
    return root_index, tuple(nodes)
)PY";

enum class TKind : uint8_t {
  Any, None, Bool, Int, Float, Str, Optional, Union, Literal, Enum,
  List, Set, FrozenSet, VarTuple, Tuple, Dict, Dataclass, TypedDict, NamedTuple
};

const char *const json_kind_names[7] = {"null", "bool", "int", "float", "str", "array", "object"};

struct TField {
  PyObject *name = nullptr; // borrowed from the description
  const char *key = nullptr;
  size_t key_len = SIZE_MAX; // SIZE_MAX: not read from JSON (init=False)
  Py_hash_t hash = 0;
  int32_t node = -1;
  uint8_t dflt_kind = 0; // 0 required, 1 default, 2 default factory, 3 optional key
  PyObject *dflt = nullptr; // borrowed
};

struct TNode {
  TKind kind = TKind::Any;
  uint8_t mask = 127; // JSON kinds accepted (bit k: json_kind_names[k])
  int32_t child = -1;
  int32_t dispatch[7] = {-1, -1, -1, -1, -1, -1, -1};
  std::vector<int32_t> items;
  std::vector<TField> fields;
  PyObject *cls = nullptr;      // borrowed
  PyObject *values = nullptr;   // borrowed: literal values, enum value -> member
  PyObject *defaults = nullptr; // borrowed: NamedTuple defaults
  bool post_init = false;
};

struct TypePlan {
  std::vector<TNode> nodes;
  int32_t root = 0;
  PyObject *desc = nullptr; // keeps the borrowed objects alive
  ~TypePlan() { Py_XDECREF(desc); }
};

void typed_plan_free(PyObject *capsule) {
  delete static_cast<TypePlan *>(PyCapsule_GetPointer(capsule, nullptr));
}

std::atomic<bool> typed_ready{false};
std::mutex typed_init_mutex;

// Runs the Python helper on first use (it imports typing, dataclasses and
// enum, which a plain loads does not need). Threads that get there together
// all run it; the first to finish publishes its objects.
bool typed_init() {
  if (typed_ready.load(std::memory_order_acquire)) {
    return true;
  }
  PyObject *globals = PyDict_New();
  PyObject *builtins = PyImport_ImportModule("builtins");
  if (globals == nullptr || builtins == nullptr ||
      PyDict_SetItemString(globals, "__builtins__", builtins) < 0) {
    Py_XDECREF(globals);
    Py_XDECREF(builtins);
    return false;
  }
  Py_DECREF(builtins);
  PyObject *r = PyRun_String(typed_describe_source, Py_file_input, globals, globals);
  if (r == nullptr) {
    Py_DECREF(globals);
    return false;
  }
  Py_DECREF(r);
  PyObject *fn = Py_XNewRef(PyDict_GetItemString(globals, "describe"));
  Py_DECREF(globals);
  PyObject *cache = PyDict_New();
  PyObject *post_init = PyUnicode_InternFromString("__post_init__");
  PyObject *empty = PyTuple_New(0);
  if (fn == nullptr || cache == nullptr || post_init == nullptr || empty == nullptr) {
    Py_XDECREF(fn);
    Py_XDECREF(cache);
    Py_XDECREF(post_init);
    Py_XDECREF(empty);
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(typed_init_mutex);
    if (!typed_ready.load(std::memory_order_relaxed)) {
      typed_describe = fn;
      typed_cache = cache;
      typed_post_init = post_init;
      typed_empty_tuple = empty;
      typed_ready.store(true, std::memory_order_release);
      return true;
    }
  }
  Py_DECREF(fn);
  Py_DECREF(cache);
  Py_DECREF(post_init);
  Py_DECREF(empty);
  return true;
}

bool compile_fields(TNode &n, PyObject *fields) {
  Py_ssize_t nf = PyTuple_GET_SIZE(fields);
  n.fields.resize(size_t(nf));
  for (Py_ssize_t f = 0; f < nf; f++) {
    PyObject *t = PyTuple_GET_ITEM(fields, f);
    TField &tf = n.fields[size_t(f)];
    tf.name = PyTuple_GET_ITEM(t, 0);
    tf.node = int32_t(PyLong_AsLong(PyTuple_GET_ITEM(t, 1)));
    tf.dflt_kind = uint8_t(PyLong_AsLong(PyTuple_GET_ITEM(t, 2)));
    tf.dflt = PyTuple_GET_ITEM(t, 3);
    tf.hash = PyObject_Hash(tf.name);
    if (tf.hash == -1) {
      return false;
    }
    if (tf.node >= 0) {
      Py_ssize_t len;
      tf.key = PyUnicode_AsUTF8AndSize(tf.name, &len);
      if (tf.key == nullptr) {
        return false;
      }
      tf.key_len = size_t(len);
    }
  }
  return true;
}

TypePlan *compile_plan(PyObject *type) {
  PyObject *desc = PyObject_CallOneArg(typed_describe, type);
  if (desc == nullptr) {
    return nullptr;
  }
  TypePlan *plan = new (std::nothrow) TypePlan();
  if (plan == nullptr) {
    Py_DECREF(desc);
    PyErr_NoMemory();
    return nullptr;
  }
  plan->desc = desc;
  plan->root = int32_t(PyLong_AsLong(PyTuple_GET_ITEM(desc, 0)));
  PyObject *nodes = PyTuple_GET_ITEM(desc, 1);
  Py_ssize_t count = PyTuple_GET_SIZE(nodes);
  plan->nodes.resize(size_t(count));
  static const struct {
    const char *name;
    TKind kind;
  } kinds[] = {{"any", TKind::Any},           {"none", TKind::None},
               {"bool", TKind::Bool},         {"int", TKind::Int},
               {"float", TKind::Float},       {"str", TKind::Str},
               {"optional", TKind::Optional}, {"union", TKind::Union},
               {"literal", TKind::Literal},   {"enum", TKind::Enum},
               {"list", TKind::List},         {"set", TKind::Set},
               {"frozenset", TKind::FrozenSet}, {"vartuple", TKind::VarTuple},
               {"tuple", TKind::Tuple},       {"dict", TKind::Dict},
               {"dataclass", TKind::Dataclass}, {"typeddict", TKind::TypedDict},
               {"namedtuple", TKind::NamedTuple}};
  for (Py_ssize_t i = 0; i < count; i++) {
    PyObject *t = PyTuple_GET_ITEM(nodes, i);
    TNode &n = plan->nodes[size_t(i)];
    const char *name = PyUnicode_AsUTF8(PyTuple_GET_ITEM(t, 0));
    for (auto &k : kinds) {
      if (strcmp(name, k.name) == 0) {
        n.kind = k.kind;
      }
    }
    n.mask = uint8_t(PyLong_AsLong(PyTuple_GET_ITEM(t, 1)));
    switch (n.kind) {
    case TKind::Optional:
    case TKind::List:
    case TKind::Set:
    case TKind::FrozenSet:
    case TKind::VarTuple:
    case TKind::Dict:
      n.child = int32_t(PyLong_AsLong(PyTuple_GET_ITEM(t, 2)));
      break;
    case TKind::Union: {
      PyObject *table = PyTuple_GET_ITEM(t, 2);
      for (int k = 0; k < 7; k++) {
        n.dispatch[k] = int32_t(PyLong_AsLong(PyTuple_GET_ITEM(table, k)));
      }
      break;
    }
    case TKind::Literal:
      n.values = PyTuple_GET_ITEM(t, 2);
      break;
    case TKind::Enum:
      n.cls = PyTuple_GET_ITEM(t, 2);
      n.values = PyTuple_GET_ITEM(t, 3);
      break;
    case TKind::Tuple: {
      PyObject *items = PyTuple_GET_ITEM(t, 2);
      for (Py_ssize_t k = 0; k < PyTuple_GET_SIZE(items); k++) {
        n.items.push_back(int32_t(PyLong_AsLong(PyTuple_GET_ITEM(items, k))));
      }
      break;
    }
    case TKind::Dataclass:
      n.cls = PyTuple_GET_ITEM(t, 2);
      n.post_init = PyObject_IsTrue(PyTuple_GET_ITEM(t, 4)) == 1;
      if (!compile_fields(n, PyTuple_GET_ITEM(t, 3))) {
        delete plan;
        return nullptr;
      }
      break;
    case TKind::TypedDict:
      n.cls = PyTuple_GET_ITEM(t, 2);
      if (!compile_fields(n, PyTuple_GET_ITEM(t, 3))) {
        delete plan;
        return nullptr;
      }
      break;
    case TKind::NamedTuple: {
      n.cls = PyTuple_GET_ITEM(t, 2);
      PyObject *items = PyTuple_GET_ITEM(t, 3);
      for (Py_ssize_t k = 0; k < PyTuple_GET_SIZE(items); k++) {
        n.items.push_back(int32_t(PyLong_AsLong(PyTuple_GET_ITEM(items, k))));
      }
      n.defaults = PyTuple_GET_ITEM(t, 4);
      break;
    }
    default:
      break;
    }
  }
  return plan;
}

// A new reference to the capsule holding the plan of type.
PyObject *get_plan(PyObject *type) {
  if (!typed_init()) {
    return nullptr;
  }
  PyObject *capsule = nullptr;
  bool hashable = true;
#if PY_VERSION_HEX >= 0x030D0000
  int found = PyDict_GetItemRef(typed_cache, type, &capsule);
  if (found < 0) {
    if (!PyErr_ExceptionMatches(PyExc_TypeError)) {
      return nullptr;
    }
    PyErr_Clear(); // unhashable: not cached
    hashable = false;
  }
#else
  capsule = Py_XNewRef(PyDict_GetItemWithError(typed_cache, type));
  if (capsule == nullptr && PyErr_Occurred()) {
    if (!PyErr_ExceptionMatches(PyExc_TypeError)) {
      return nullptr;
    }
    PyErr_Clear();
    hashable = false;
  }
#endif
  if (capsule != nullptr) {
    return capsule;
  }
  TypePlan *plan = compile_plan(type);
  if (plan == nullptr) {
    return nullptr;
  }
  capsule = PyCapsule_New(plan, nullptr, typed_plan_free);
  if (capsule == nullptr) {
    delete plan;
    return nullptr;
  }
  if (hashable) {
    PyObject *kept = PyDict_SetDefault(typed_cache, type, capsule); // borrowed
    if (kept == nullptr) {
      Py_DECREF(capsule);
      return nullptr;
    }
    Py_SETREF(capsule, Py_NewRef(kept));
  }
  return capsule;
}

inline const TypePlan &plan_of(PyObject *capsule) {
  return *static_cast<const TypePlan *>(PyCapsule_GetPointer(capsule, nullptr));
}

// Where a validation error happened, innermost first.
struct PathItem {
  const char *key; // nullptr for an index
  size_t n;        // the key's length, or the index
};

struct TypedError {
  std::string msg;   // our message (no Python exception set), when ours
  bool ours = false;
  std::vector<PathItem> path;

  void set(std::string m) {
    msg = std::move(m);
    ours = true;
  }
};

// The source of the values: the tape of a document...
struct TapeSource {
  using Ref = size_t;
  Builder b;
  explicit TapeSource(const document &doc) : b(doc) {}

  int kind(size_t i) const {
    switch (uint8_t(b.tape[i] >> 56)) {
    case 'n':
      return 0;
    case 't':
    case 'f':
      return 1;
    case 'l':
    case 'u':
    case 'Z':
      return 2;
    case 'd':
      return 3;
    case '"':
      return 4;
    case '[':
      return 5;
    default:
      return 6;
    }
  }
  PyObject *any(size_t i) { return b.build(i); }
  PyObject *real(size_t i) {
    uint64_t word = b.tape[i];
    switch (uint8_t(word >> 56)) {
    case 'd': {
      double v;
      memcpy(&v, &b.tape[i + 1], 8);
      return PyFloat_FromDouble(v);
    }
    case 'l': {
      int64_t v;
      memcpy(&v, &b.tape[i + 1], 8);
      return PyFloat_FromDouble(double(v));
    }
    case 'u':
      return PyFloat_FromDouble(double(b.tape[i + 1]));
    default: { // a big integer
      PyObject *n = b.build(i);
      PyObject *f = n ? PyNumber_Float(n) : nullptr;
      Py_XDECREF(n);
      return f;
    }
    }
  }
  size_t length(size_t i) const { return count_of(b.tape, i); }
  template <class F> bool elements(size_t i, F &&f) {
    size_t end = size_t(b.tape[i] & 0xFFFFFFFF) - 1;
    size_t k = 0;
    for (size_t j = i + 1; j < end; j = skip(b.tape, j), k++) {
      if (!f(k, j)) {
        return false;
      }
    }
    return true;
  }
  // f(key, key_len, key_ref, value)
  template <class F> bool members(size_t i, F &&f) {
    size_t end = size_t(b.tape[i] & 0xFFFFFFFF) - 1;
    for (size_t j = i + 1; j < end; j = skip(b.tape, j + 1)) {
      size_t len;
      const char *s = b.str_at(b.tape[j], &len);
      if (!f(s, len, j, j + 1)) {
        return false;
      }
    }
    return true;
  }
  PyObject *key(size_t j) {
    size_t len;
    const char *s = b.str_at(b.tape[j], &len);
    return make_key(b.keys, s, len);
  }
};

// ... or the Python objects that json made.
struct PySource {
  using Ref = PyObject *;
  int kind(PyObject *o) const {
    if (o == Py_None) {
      return 0;
    }
    if (PyBool_Check(o)) {
      return 1;
    }
    if (PyLong_Check(o)) {
      return 2;
    }
    if (PyFloat_Check(o)) {
      return 3;
    }
    if (PyUnicode_Check(o)) {
      return 4;
    }
    if (PyList_Check(o)) {
      return 5;
    }
    return 6;
  }
  PyObject *any(PyObject *o) { return Py_NewRef(o); }
  PyObject *real(PyObject *o) { return PyNumber_Float(o); }
  size_t length(PyObject *o) const {
    return size_t(PyList_Check(o) ? PyList_GET_SIZE(o) : PyDict_GET_SIZE(o));
  }
  template <class F> bool elements(PyObject *o, F &&f) {
    for (Py_ssize_t k = 0; k < PyList_GET_SIZE(o); k++) {
      if (!f(size_t(k), PyList_GET_ITEM(o, k))) {
        return false;
      }
    }
    return true;
  }
  template <class F> bool members(PyObject *o, F &&f) {
    Py_ssize_t pos = 0;
    PyObject *k, *v;
    while (PyDict_Next(o, &pos, &k, &v)) {
      Py_ssize_t len;
      const char *s = PyUnicode_AsUTF8AndSize(k, &len);
      if (s == nullptr || !f(s, size_t(len), k, v)) {
        return false;
      }
    }
    return true;
  }
  PyObject *key(PyObject *k) { return Py_NewRef(k); }
};

inline size_t find_field(const TNode &n, const char *s, size_t len, size_t guess) {
  size_t nf = n.fields.size();
  if (guess < nf && n.fields[guess].key_len == len && memcmp(n.fields[guess].key, s, len) == 0) {
    return guess;
  }
  for (size_t f = 0; f < nf; f++) {
    if (n.fields[f].key_len == len && memcmp(n.fields[f].key, s, len) == 0) {
      return f;
    }
  }
  return nf;
}

// The values of the fields of an object, by field.
struct FieldValues {
  PyObject *small[16];
  std::vector<PyObject *> big;
  PyObject **v;
  size_t n;
  explicit FieldValues(size_t count) : n(count) {
    if (count <= 16) {
      v = small;
    } else {
      big.resize(count);
      v = big.data();
    }
    for (size_t f = 0; f < count; f++) {
      v[f] = nullptr;
    }
  }
  ~FieldValues() {
    for (size_t f = 0; f < n; f++) {
      Py_XDECREF(v[f]);
    }
  }
};

template <class Src> struct TypedDecoder {
  const TypePlan &plan;
  Src &src;
  TypedError &err;
  using Ref = typename Src::Ref;

  PyObject *expected(const TNode &n, int k) {
    std::string m = "Expected `";
    bool first = true;
    for (int j = 0; j < 7; j++) {
      // float accepts int: it shows as float only.
      if ((n.mask >> j & 1) && !(j == 2 && (n.mask & 8))) {
        m += first ? "" : " | ";
        m += json_kind_names[j];
        first = false;
      }
    }
    m += "`, got `";
    m += json_kind_names[k];
    m += "`";
    err.set(std::move(m));
    return nullptr;
  }

  PyObject *invalid_value(PyObject *x) {
    PyObject *r = PyObject_Repr(x);
    Py_DECREF(x);
    const char *s = r ? PyUnicode_AsUTF8(r) : nullptr;
    if (s == nullptr) {
      Py_XDECREF(r);
      return nullptr;
    }
    err.set(std::string("Invalid value ") + s);
    Py_DECREF(r);
    return nullptr;
  }

  PyObject *decode(int32_t ni, Ref v) {
    const TNode &n = plan.nodes[size_t(ni)];
    int k = src.kind(v);
    if (!(n.mask >> k & 1)) {
      return expected(n, k);
    }
    switch (n.kind) {
    case TKind::Any:
    case TKind::None:
    case TKind::Bool:
    case TKind::Int:
    case TKind::Str:
      return src.any(v);
    case TKind::Float:
      return src.real(v);
    case TKind::Optional:
      return k == 0 ? Py_NewRef(Py_None) : decode(n.child, v);
    case TKind::Union:
      return decode(n.dispatch[k], v);
    case TKind::Literal: {
      PyObject *x = src.any(v);
      if (x == nullptr) {
        return nullptr;
      }
      int c = PySet_Contains(n.values, x);
      if (c == 1) {
        return x;
      }
      if (c < 0) {
        Py_DECREF(x);
        return nullptr;
      }
      return invalid_value(x);
    }
    case TKind::Enum: {
      PyObject *x = src.any(v);
      if (x == nullptr) {
        return nullptr;
      }
      PyObject *m = PyDict_GetItemWithError(n.values, x); // borrowed
      if (m != nullptr) {
        Py_DECREF(x);
        return Py_NewRef(m);
      }
      if (PyErr_Occurred()) {
        Py_DECREF(x);
        return nullptr;
      }
      // Lets the enum's _missing_ decide.
      m = PyObject_CallOneArg(n.cls, x);
      if (m == nullptr && PyErr_ExceptionMatches(PyExc_ValueError)) {
        PyErr_Clear();
        return invalid_value(x);
      }
      Py_DECREF(x);
      return m;
    }
    case TKind::List:
    case TKind::VarTuple: {
      size_t len = src.length(v);
      bool list = n.kind == TKind::List;
      PyObject *out = list ? PyList_New(Py_ssize_t(len)) : PyTuple_New(Py_ssize_t(len));
      if (out == nullptr) {
        return nullptr;
      }
      bool ok = src.elements(v, [&](size_t j, Ref e) {
        PyObject *x = decode(n.child, e);
        if (x == nullptr) {
          err.path.push_back({nullptr, j});
          return false;
        }
        if (list) {
          PyList_SET_ITEM(out, Py_ssize_t(j), x);
        } else {
          PyTuple_SET_ITEM(out, Py_ssize_t(j), x);
        }
        return true;
      });
      if (!ok) {
        Py_DECREF(out);
        return nullptr;
      }
      return out;
    }
    case TKind::Tuple:
    case TKind::NamedTuple:
      return decode_tuple(n, v);
    case TKind::Set:
    case TKind::FrozenSet: {
      PyObject *out = n.kind == TKind::Set ? PySet_New(nullptr) : PyFrozenSet_New(nullptr);
      if (out == nullptr) {
        return nullptr;
      }
      bool ok = src.elements(v, [&](size_t j, Ref e) {
        PyObject *x = decode(n.child, e);
        int rc = x == nullptr ? -1 : PySet_Add(out, x);
        Py_XDECREF(x);
        if (rc < 0) {
          err.path.push_back({nullptr, j});
          return false;
        }
        return true;
      });
      if (!ok) {
        Py_DECREF(out);
        return nullptr;
      }
      return out;
    }
    case TKind::Dict: {
      size_t count = src.length(v);
      PyObject *out = count > 5 ? _PyDict_NewPresized(Py_ssize_t(count)) : PyDict_New();
      if (out == nullptr) {
        return nullptr;
      }
      bool ok = src.members(v, [&](const char *s, size_t len, Ref kr, Ref e) {
        PyObject *x = decode(n.child, e);
        if (x == nullptr) {
          err.path.push_back({s, len});
          return false;
        }
        // The keys come with their hash (make_key, or the keys of a dict).
        PyObject *key = src.key(kr);
        int rc = key == nullptr ? -1
                                : new_dict_set_item(out, key, x,
                                                    reinterpret_cast<PyASCIIObject *>(key)->hash);
        Py_XDECREF(key);
        Py_DECREF(x);
        return rc == 0;
      });
      if (!ok) {
        Py_DECREF(out);
        return nullptr;
      }
      return out;
    }
    case TKind::Dataclass:
    case TKind::TypedDict:
      return decode_object(n, v);
    }
    return nullptr;
  }

  PyObject *decode_tuple(const TNode &n, Ref v) {
    size_t len = src.length(v);
    size_t size = n.items.size();
    size_t ndefaults = n.defaults ? size_t(PyTuple_GET_SIZE(n.defaults)) : 0;
    if (len > size || len + ndefaults < size) {
      std::string m = "Expected `array` of length ";
      m += ndefaults ? std::to_string(size - ndefaults) + " to " + std::to_string(size)
                     : std::to_string(size);
      m += ", got " + std::to_string(len);
      err.set(std::move(m));
      return nullptr;
    }
    PyObject *out = PyTuple_New(Py_ssize_t(size));
    if (out == nullptr) {
      return nullptr;
    }
    bool ok = src.elements(v, [&](size_t j, Ref e) {
      PyObject *x = decode(n.items[j], e);
      if (x == nullptr) {
        err.path.push_back({nullptr, j});
        return false;
      }
      PyTuple_SET_ITEM(out, Py_ssize_t(j), x);
      return true;
    });
    if (!ok) {
      Py_DECREF(out);
      return nullptr;
    }
    for (size_t j = len; j < size; j++) {
      PyTuple_SET_ITEM(out, Py_ssize_t(j),
                       Py_NewRef(PyTuple_GET_ITEM(n.defaults, Py_ssize_t(j - (size - ndefaults)))));
    }
    if (n.kind == TKind::Tuple) {
      return out;
    }
    PyObject *r = PyObject_Call(n.cls, out, nullptr);
    Py_DECREF(out);
    return r;
  }

  PyObject *decode_object(const TNode &n, Ref v) {
    size_t nf = n.fields.size();
    FieldValues vals(nf);
    size_t guess = 0;
    bool ok = src.members(v, [&](const char *s, size_t len, Ref, Ref e) {
      size_t f = find_field(n, s, len, guess);
      if (f == nf) {
        return true; // unknown keys are ignored
      }
      guess = f + 1;
      PyObject *x = decode(n.fields[f].node, e);
      if (x == nullptr) {
        err.path.push_back({s, len});
        return false;
      }
      Py_XSETREF(vals.v[f], x);
      return true;
    });
    if (!ok) {
      return nullptr;
    }
    for (size_t f = 0; f < nf; f++) {
      if (vals.v[f] != nullptr) {
        continue;
      }
      const TField &tf = n.fields[f];
      switch (tf.dflt_kind) {
      case 0:
        if (tf.node >= 0) {
          err.set(std::string("Object missing required field `") +
                  std::string(tf.key, tf.key_len) + "`");
          return nullptr;
        }
        break;
      case 1:
        vals.v[f] = Py_NewRef(tf.dflt);
        break;
      case 2:
        vals.v[f] = PyObject_CallNoArgs(tf.dflt);
        if (vals.v[f] == nullptr) {
          return nullptr;
        }
        break;
      default: // a key that a TypedDict may omit
        break;
      }
    }
    if (n.kind == TKind::TypedDict) {
      PyObject *out = PyDict_New();
      if (out == nullptr) {
        return nullptr;
      }
      for (size_t f = 0; f < nf; f++) {
        if (vals.v[f] != nullptr &&
            new_dict_set_item(out, n.fields[f].name, vals.v[f], n.fields[f].hash) < 0) {
          Py_DECREF(out);
          return nullptr;
        }
      }
      return out;
    }
    PyTypeObject *tp = reinterpret_cast<PyTypeObject *>(n.cls);
    PyObject *out = tp->tp_new(tp, typed_empty_tuple, nullptr);
    if (out == nullptr) {
      return nullptr;
    }
    // Like the dataclass's __init__, but past a frozen dataclass's __setattr__.
    for (size_t f = 0; f < nf; f++) {
      if (vals.v[f] != nullptr && PyObject_GenericSetAttr(out, n.fields[f].name, vals.v[f]) < 0) {
        Py_DECREF(out);
        return nullptr;
      }
    }
    if (n.post_init) {
      PyObject *r = PyObject_CallMethodNoArgs(out, typed_post_init);
      if (r == nullptr) {
        Py_DECREF(out);
        return nullptr;
      }
      Py_DECREF(r);
    }
    return out;
  }
};

// Raises the error of a failed typed decoding: ours, or a TypeError or
// ValueError from the user's code (__post_init__, an enum's _missing_), as a
// ValidationError with the path. Other exceptions are left as they are.
void raise_typed_error(TypedError &err) {
  std::string msg;
  PyObject *cause = nullptr;
  if (err.ours) {
    msg = err.msg;
  } else {
    if (!PyErr_ExceptionMatches(PyExc_TypeError) && !PyErr_ExceptionMatches(PyExc_ValueError)) {
      return;
    }
    if (PyErr_ExceptionMatches(ValidationError)) {
      return;
    }
    PyObject *t, *tb;
    PyErr_Fetch(&t, &cause, &tb);
    PyErr_NormalizeException(&t, &cause, &tb);
    if (tb != nullptr) {
      PyException_SetTraceback(cause, tb);
    }
    Py_XDECREF(t);
    Py_XDECREF(tb);
    PyObject *s = PyObject_Str(cause);
    const char *c = s ? PyUnicode_AsUTF8(s) : nullptr;
    if (c == nullptr) {
      Py_XDECREF(s);
      Py_DECREF(cause);
      return;
    }
    msg = c;
    Py_DECREF(s);
  }
  if (!err.path.empty()) {
    msg += " - at `$";
    for (size_t i = err.path.size(); i-- > 0;) {
      const PathItem &p = err.path[i];
      if (p.key == nullptr) {
        msg += "[" + std::to_string(p.n) + "]";
      } else {
        msg += ".";
        msg.append(p.key, p.n);
      }
    }
    msg += "`";
  }
  PyObject *m = PyUnicode_DecodeUTF8(msg.data(), Py_ssize_t(msg.size()), "replace");
  PyObject *exc = m ? PyObject_CallOneArg(ValidationError, m) : nullptr;
  Py_XDECREF(m);
  if (exc == nullptr) {
    Py_XDECREF(cause);
    return;
  }
  if (cause != nullptr) {
    PyException_SetCause(exc, cause); // steals cause
  }
  PyErr_SetObject(ValidationError, exc);
  Py_DECREF(exc);
}

// The document on the tape (root at index 1) as an instance of the plan's type.
PyObject *typed_build(const document &doc, PyObject *capsule) {
  const TypePlan &plan = plan_of(capsule);
  TapeSource src(doc);
  TypedError err;
#ifndef Py_GIL_DISABLED
  int gc_was_enabled = PyGC_Disable();
#endif
  PyObject *r = TypedDecoder<TapeSource>{plan, src, err}.decode(plan.root, 1);
#ifndef Py_GIL_DISABLED
  if (gc_was_enabled) {
    PyGC_Enable();
  }
#endif
  if (r == nullptr) {
    raise_typed_error(err);
  }
  return r;
}

// The same, from what json decoded.
PyObject *typed_convert(PyObject *value, PyObject *capsule) {
  const TypePlan &plan = plan_of(capsule);
  PySource src;
  TypedError err;
  PyObject *r = TypedDecoder<PySource>{plan, src, err}.decode(plan.root, value);
  if (r == nullptr) {
    raise_typed_error(err);
  }
  return r;
}
