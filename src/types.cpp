// mu-lan 类型模型
//
// 设计：TypePtr 指向不可变的值对象；Struct 的 fields 在先建占位、后填充
// （对应 Python 版 StructType 的两阶段构造），因此这里不用 const。
#include "types.hpp"

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ---------------------------------------------------------------- 基本类型表
// 与 Python 的 INT_BITS / INT_SIGNED 一一对应（顺序即 prim_names() 的顺序）。
struct IntInfo {
    const char* name;
    int bits;
    bool is_signed;
};

constexpr std::array<IntInfo, 11> kInts{{
    {"byte", 8, true},
    {"ubyte", 8, false},
    {"char", 8, false},
    {"short", 16, true},
    {"ushort", 16, false},
    {"int", 32, true},
    {"uint", 32, false},
    {"long", 64, true},
    {"ulong", 64, false},
    {"usize", 64, false},
    {"isize", 64, true},
}};

const IntInfo* find_int(const std::string& n) {
    for (const auto& p : kInts) {
        if (n == p.name) return &p;
    }
    return nullptr;
}

bool is_float_name(const std::string& n) { return n == "float" || n == "double"; }

// 类型文本化的兜底：指针/数组的 elem 理论上不会为空，但错误路径上不要崩。
std::string elem_str(const TypePtr& t) { return t ? t->str() : std::string("void"); }

}  // namespace

// ---------------------------------------------------------------- 工厂函数

TypePtr Type::make_void() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Void;
    return t;
}

TypePtr Type::make_prim(const std::string& name) {
    // 基本类型不可变，按名字缓存可让 `prim("int")` 与 t_int() 共享同一对象。
    // 编译器是单线程的，函数内 static 无需加锁。
    static std::unordered_map<std::string, TypePtr> cache;
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Prim;
    t->name = name;
    cache.emplace(name, t);
    return t;
}

TypePtr Type::make_string() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::String;
    return t;
}

TypePtr Type::make_ptr(const TypePtr& elem, bool readonly) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Ptr;
    t->elem = elem;
    t->readonly = readonly;
    return t;
}

TypePtr Type::make_array(const TypePtr& elem, size_t size) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Array;
    t->elem = elem;
    t->array_size = size;
    return t;
}

TypePtr Type::make_slice(const TypePtr& elem, bool readonly) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Slice;
    t->elem = elem;
    t->readonly = readonly;
    return t;
}

TypePtr Type::make_struct(const std::string& unit, const std::string& name) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Struct;
    t->unit = unit;
    t->name = name;
    return t;
}

TypePtr Type::make_enum(const std::string& unit, const std::string& name) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Enum;
    t->unit = unit;
    t->name = name;
    return t;
}

TypePtr Type::make_class(const std::string& unit, const std::string& name) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Class;
    t->unit = unit;
    t->name = name;
    return t;
}

TypePtr Type::make_interface(const std::string& unit, const std::string& name) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Interface;
    t->unit = unit;
    t->name = name;
    return t;
}

TypePtr Type::make_module(const std::string& unit) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Module;
    t->unit = unit;
    return t;
}

TypePtr Type::make_null() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Null;
    return t;
}

TypePtr Type::make_range() {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Range;
    return t;
}

// ---------------------------------------------------------------- 文本化

std::string Type::str() const {
    switch (kind) {
        case TypeKind::Void:
            return "void";
        case TypeKind::Prim:
            return name;
        case TypeKind::String:
            return "string";
        case TypeKind::Ptr: {
            std::string base = elem_str(elem) + "*";
            return readonly ? "readonly " + base : base;
        }
        case TypeKind::Array:
            return elem_str(elem) + "[" + std::to_string(array_size) + "]";
        case TypeKind::Slice: {
            std::string base = elem_str(elem) + "[]";
            return readonly ? "readonly " + base : base;
        }
        case TypeKind::Struct:
        case TypeKind::Enum:
        case TypeKind::Class:
        case TypeKind::Interface:
            return unit.empty() ? name : unit + "." + name;
        case TypeKind::Module:
            return unit;
        case TypeKind::Null:
            return "null";
        case TypeKind::Range:
            return "range";
    }
    return "?";
}

// ---------------------------------------------------------------- 判定

bool Type::is_integer() const {
    if (kind == TypeKind::Enum) return true;  // 枚举底层是 int
    if (kind != TypeKind::Prim) return false;
    return find_int(name) != nullptr;
}

bool Type::is_float() const { return kind == TypeKind::Prim && is_float_name(name); }

bool Type::is_bool() const { return kind == TypeKind::Prim && name == "bool"; }

bool Type::is_numeric() const { return is_integer() || is_float(); }

int Type::bits() const {
    if (kind != TypeKind::Prim) return 0;  // 枚举没有位宽概念（Python 版此处会 AttributeError）
    if (is_float()) return name == "float" ? 32 : 64;
    if (const IntInfo* p = find_int(name)) return p->bits;
    return 0;
}

bool Type::is_signed() const {
    if (kind != TypeKind::Prim) return true;  // 与 INT_SIGNED.get(name, True) 一致
    if (const IntInfo* p = find_int(name)) return p->is_signed;
    return true;
}

TypePtr Type::field_type(const std::string& fname) const {
    if (kind != TypeKind::Struct && kind != TypeKind::Class) return nullptr;
    for (const auto& f : fields) {
        if (f.name == fname) return f.type;
    }
    return nullptr;
}

int Type::field_index(const std::string& fname) const {
    if (kind != TypeKind::Struct && kind != TypeKind::Class) return -1;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].name == fname) return static_cast<int>(i);
    }
    return -1;
}

// ---------------------------------------------------------------- 单例

TypePtr t_void() {
    static TypePtr t = Type::make_void();
    return t;
}

TypePtr t_string() {
    static TypePtr t = Type::make_string();
    return t;
}

TypePtr t_int() {
    static TypePtr t = Type::make_prim("int");
    return t;
}

TypePtr t_bool() {
    static TypePtr t = Type::make_prim("bool");
    return t;
}

TypePtr t_null() {
    static TypePtr t = Type::make_null();
    return t;
}

TypePtr t_range() {
    static TypePtr t = Type::make_range();
    return t;
}

// ---------------------------------------------------------------- 基本类型名

const std::vector<std::string>& prim_names() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        v.reserve(kInts.size() + 3);
        for (const auto& p : kInts) v.emplace_back(p.name);
        v.emplace_back("float");
        v.emplace_back("double");
        v.emplace_back("bool");
        return v;
    }();
    return names;
}

bool is_prim_name(const std::string& n) {
    for (const auto& s : prim_names()) {
        if (s == n) return true;
    }
    return false;
}

// ---------------------------------------------------------------- 类型相等

bool same_type(const TypePtr& a, const TypePtr& b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case TypeKind::Void:
        case TypeKind::String:
        case TypeKind::Null:
        case TypeKind::Range:
            return true;
        case TypeKind::Prim:
            return a->name == b->name;
        case TypeKind::Ptr:
        case TypeKind::Slice:
            return a->readonly == b->readonly && same_type(a->elem, b->elem);
        case TypeKind::Array:
            return a->array_size == b->array_size && same_type(a->elem, b->elem);
        case TypeKind::Struct:
        case TypeKind::Enum:
        case TypeKind::Class:
        case TypeKind::Interface:
            // 结构体/枚举/class/interface 按 (unit, name) 比较
            return a->unit == b->unit && a->name == b->name;
        case TypeKind::Module:
            return a->unit == b->unit;
    }
    return false;
}

// ---------------------------------------------------------------- 转换规则

bool is_class_type(const TypePtr& t) { return t && t->kind == TypeKind::Class; }
bool is_interface_type(const TypePtr& t) { return t && t->kind == TypeKind::Interface; }
bool is_reference_type(const TypePtr& t) { return is_class_type(t) || is_interface_type(t); }

bool is_subclass_of(const TypePtr& sub, const TypePtr& base) {
    if (!sub || !base || base->kind != TypeKind::Class) return false;
    TypePtr cur = sub;
    int guard = 0;
    while (cur && cur->kind == TypeKind::Class && guard++ < 1024) {
        if (same_type(cur, base)) return true;
        cur = cur->base;
    }
    return false;
}

static bool iface_extends(const TypePtr& a, const TypePtr& b, int depth) {
    if (!a || !b || depth > 256) return false;
    if (same_type(a, b)) return true;
    for (auto& i : a->interfaces) {
        if (iface_extends(i, b, depth + 1)) return true;
    }
    return false;
}

bool implements_interface(const TypePtr& t, const TypePtr& iface) {
    if (!t || !iface || iface->kind != TypeKind::Interface) return false;
    if (t->kind == TypeKind::Interface) return iface_extends(t, iface, 0);
    if (t->kind != TypeKind::Class) return false;

    TypePtr cur = t;
    int guard = 0;
    while (cur && cur->kind == TypeKind::Class && guard++ < 1024) {
        for (auto& i : cur->interfaces) {
            if (iface_extends(i, iface, 0)) return true;
        }
        cur = cur->base;
    }
    return false;
}

bool widens_int(const TypePtr& src, const TypePtr& dst) {
    if (!src || !dst) return false;
    if (src->kind != TypeKind::Prim || dst->kind != TypeKind::Prim) return false;
    if (!src->is_integer() || !dst->is_integer()) return false;
    if (src->name == dst->name) return true;

    const int sb = src->bits();
    const int db = dst->bits();
    const bool ss = src->is_signed();
    const bool ds = dst->is_signed();

    if (ss && ds) return db > sb;
    if (!ss && !ds) return db > sb;
    if (!ss && ds) return db > sb;
    return false;  // 有符号 -> 无符号：一律不允许隐式
}

bool implicitly_convertible(const TypePtr& src, const TypePtr& dst) {
    if (same_type(src, dst)) return true;
    if (!src || !dst) return false;

    // null -> 指针 / 切片 / string / class / interface
    if (src->kind == TypeKind::Null) {
        return dst->kind == TypeKind::Ptr || dst->kind == TypeKind::Slice ||
               dst->kind == TypeKind::String || dst->kind == TypeKind::Class ||
               dst->kind == TypeKind::Interface;
    }

    // class / interface 子类型（单继承 + 接口）
    if (is_subclass_of(src, dst)) return true;
    if (implements_interface(src, dst)) return true;

    // 指针：T* -> void*；T* -> readonly T*；去掉 readonly 必须显式 as
    if (src->kind == TypeKind::Ptr && dst->kind == TypeKind::Ptr) {
        if (!(dst->readonly || !src->readonly)) return false;
        if (dst->elem && dst->elem->kind == TypeKind::Void) return true;
        return same_type(src->elem, dst->elem);
    }

    // 数组 -> 切片
    if (src->kind == TypeKind::Array && dst->kind == TypeKind::Slice) {
        return same_type(src->elem, dst->elem);
    }

    // 切片：T[] -> readonly T[]；反过来必须显式 as
    if (src->kind == TypeKind::Slice && dst->kind == TypeKind::Slice) {
        if (!same_type(src->elem, dst->elem)) return false;
        return dst->readonly || !src->readonly;
    }

    // string -> readonly char* / readonly char[] | byte[] | ubyte[]
    if (src->kind == TypeKind::String) {
        if (dst->kind == TypeKind::Ptr && dst->readonly) {
            if (!dst->elem) return false;
            if (dst->elem->kind == TypeKind::Void) return true;
            return dst->elem->kind == TypeKind::Prim && dst->elem->name == "char";
        }
        if (dst->kind == TypeKind::Slice && dst->readonly) {
            if (!dst->elem || dst->elem->kind != TypeKind::Prim) return false;
            return dst->elem->name == "char" || dst->elem->name == "byte" ||
                   dst->elem->name == "ubyte";
        }
        return false;
    }

    // 数值：只允许值域不丢失的加宽
    if (src->kind == TypeKind::Prim && dst->kind == TypeKind::Prim) {
        if (src->is_integer() && dst->is_integer()) return widens_int(src, dst);
        if (src->is_float() && dst->is_float()) {
            return dst->name == "double" && src->name == "float";
        }
        return false;
    }

    // 枚举 -> int
    if (src->kind == TypeKind::Enum && dst->kind == TypeKind::Prim) return dst->name == "int";

    return false;
}

bool promote_int_literal(long long v, const TypePtr& dst) {
    if (!dst) return false;
    // 枚举不是整数的隐式目标：需要显式 `as`
    if (dst->kind != TypeKind::Prim) return false;
    if (dst->is_float()) return true;
    if (!dst->is_integer()) return false;

    const int bits = dst->bits();
    if (bits <= 0) return false;
    if (bits >= 64) {
        // 有符号 64 位能表示全部 long long；无符号 64 位只接受非负值。
        return dst->is_signed() ? true : v >= 0;
    }
    if (dst->is_signed()) {
        const long long lo = -(1LL << (bits - 1));
        const long long hi = 1LL << (bits - 1);
        return v >= lo && v < hi;
    }
    return v >= 0 && static_cast<unsigned long long>(v) < (1ULL << bits);
}

TypePtr usual_arith(const TypePtr& a_in, const TypePtr& b_in) {
    if (!a_in || !b_in) return nullptr;
    TypePtr a = a_in;
    TypePtr b = b_in;
    if (a->kind == TypeKind::Enum) a = t_int();
    if (b->kind == TypeKind::Enum) b = t_int();
    if (!a->is_numeric() || !b->is_numeric()) return nullptr;
    if (a->kind != TypeKind::Prim || b->kind != TypeKind::Prim) return nullptr;

    if (a->is_float() || b->is_float()) {
        if (a->name == "double" || b->name == "double") return Type::make_prim("double");
        return Type::make_prim("float");
    }

    // 小于 int 的一律提升到 int（C 语义：8/16 位都能被 int 表示）
    if (a->bits() < 32) a = t_int();
    if (b->bits() < 32) b = t_int();

    if (a->bits() != b->bits()) return a->bits() > b->bits() ? a : b;

    // 同宽：都为有符号或都为无符号时取其一；混合符号按 C 语义取无符号
    if (a->is_signed() && b->is_signed()) return a;
    if (!a->is_signed() && !b->is_signed()) return a;
    return b->is_signed() ? a : b;
}
