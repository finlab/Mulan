// mu-lan 类型模型
//
// 设计：TypePtr 指向不可变的值对象；Struct 的 fields 在先建占位、后填充
// （对应 Python 版 StructType 的两阶段构造），因此这里不用 const。
#include "types.hpp"

#include <algorithm>
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

constexpr std::array<IntInfo, 10> kInts{{
    {"byte", 8, false},
    {"char", 8, true},
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

TypePtr Type::make_ptr(const TypePtr& elem, bool is_const) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Ptr;
    t->elem = elem;
    t->is_const = is_const;
    return t;
}

TypePtr Type::make_array(const TypePtr& elem, size_t size) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Array;
    t->elem = elem;
    t->array_size = size;
    return t;
}

TypePtr Type::make_slice(const TypePtr& elem, bool is_const, int rank) {
    auto t = std::make_shared<Type>();
    t->kind = TypeKind::Slice;
    t->elem = elem;
    t->is_const = is_const;
    t->rank = rank < 1 ? 1 : rank;
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
            return is_const ? "const " + name : name;
        case TypeKind::String:
            return "string";
        case TypeKind::Ptr: {
            std::string base = elem_str(elem) + "*";
            return is_const ? "const " + base : base;
        }
        case TypeKind::Array: {
            // 嵌套静态数组显示为 T[m,n,...]
            std::vector<std::string> dims;
            const Type* cur = this;
            while (cur->kind == TypeKind::Array) {
                dims.push_back(std::to_string(cur->array_size));
                cur = cur->elem.get();
            }
            std::string base = cur ? cur->str() : std::string("void");
            std::string out = base + "[";
            for (size_t i = 0; i < dims.size(); ++i) {
                if (i) out += ",";
                out += dims[i];
            }
            return out + "]";
        }
        case TypeKind::Slice: {
            std::string base = elem_str(elem) + "[" + std::string(rank - 1, ',') + "]";
            return is_const ? "const " + base : base;
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

bool Type::needs_cleanup() const {
    switch (kind) {
        case TypeKind::Class:
        case TypeKind::Interface: return true;
        case TypeKind::Struct:
            if (has_dtor) return true;
            for (const auto& f : fields) {
                if (f.weak) continue;
                if (f.type && f.type->needs_cleanup()) return true;
            }
            return false;
        case TypeKind::Array:
            return elem && elem->needs_cleanup();
        case TypeKind::Slice: return true;  // 动态数组：句柄持有 RC
        default: return false;
    }
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

// ---------------------------------------------------------------- 布局
//
// packed / align(N) 的唯一实现（codegen 的 alignOf / sizeOf 委托到这里）。
// 递归带深度上限，避免畸形类型（不存在的按值环）把编译器递归爆栈。

namespace {

size_t align_up(size_t v, size_t a) { return a ? (v + a - 1) / a * a : v; }

size_t align_impl(const TypePtr& t, int depth) {
    if (!t || depth > 64) return 1;
    switch (t->kind) {
        case TypeKind::Void: return 1;
        case TypeKind::Prim: {
            const std::string& n = t->name;
            if (n == "bool" || n == "byte" || n == "char") return 1;
            if (n == "short" || n == "ushort") return 2;
            if (n == "int" || n == "uint" || n == "float") return 4;
            return 8;
        }
        case TypeKind::Ptr:
        case TypeKind::Slice: return 8;
        case TypeKind::Enum: return 4;
        case TypeKind::Array: return align_impl(t->elem, depth + 1);
        case TypeKind::Struct: {
            if (t->is_packed) return t->align_req > 0 ? static_cast<size_t>(t->align_req) : 1;
            size_t natural = 1;
            for (const auto& f : t->fields) natural = std::max(natural, align_impl(f.type, depth + 1));
            // align(N) 要求 N >= 自然对齐（sema 已校验）；这里取 max 保证不塌陷
            if (t->align_req > 0) return std::max(natural, static_cast<size_t>(t->align_req));
            return natural;
        }
        default: return 8;
    }
}

size_t size_impl(const TypePtr& t, int depth) {
    if (!t || depth > 64) return 0;
    switch (t->kind) {
        case TypeKind::Void: return 0;
        case TypeKind::Prim: {
            const std::string& n = t->name;
            if (n == "bool" || n == "byte" || n == "char") return 1;
            if (n == "short" || n == "ushort") return 2;
            if (n == "int" || n == "uint" || n == "float") return 4;
            return 8;
        }
        case TypeKind::Ptr:
        case TypeKind::Slice: return 8;
        case TypeKind::Interface: return 16;
        case TypeKind::Enum: return 4;
        case TypeKind::Array: return size_impl(t->elem, depth + 1) * t->array_size;
        case TypeKind::Struct: {
            size_t off = 0, natural = 1;
            for (const auto& f : t->fields) {
                size_t fa = align_impl(f.type, depth + 1);
                natural = std::max(natural, fa);
                // packed：字段连续累加，无内部填充
                if (!t->is_packed) off = align_up(off, fa);
                off += size_impl(f.type, depth + 1);
            }
            if (t->is_packed) {
                // 无尾部填充；只有 align(N) 才补齐到 N 的倍数
                if (t->align_req > 0) off = align_up(off, static_cast<size_t>(t->align_req));
                return off;
            }
            size_t a = natural;
            if (t->align_req > 0) a = std::max(a, static_cast<size_t>(t->align_req));
            return align_up(off, a);
        }
        default: return 8;
    }
}

}  // namespace

size_t type_align(const TypePtr& t) { return align_impl(t, 0); }
size_t type_size(const TypePtr& t) { return size_impl(t, 0); }

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
            // C 模型：const 只是视图限定，不改变类型的相等性
            return a->rank == b->rank && same_type(a->elem, b->elem);
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
    // C 模型：const 只能放宽，不能收紧（same_type 忽略 const，必须先拦）
    if (src && dst && src->kind == dst->kind) {
        if (src->kind == TypeKind::Ptr || src->kind == TypeKind::Slice) {
            if (src->elem && dst->elem && src->elem->is_const && !dst->elem->is_const) {
                return false;
            }
        }
    }
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

    // 指针：T* -> const T*；去掉 const 必须显式 as
    if (src->kind == TypeKind::Ptr && dst->kind == TypeKind::Ptr) {
        if (src->elem && src->elem->is_const && !(dst->elem && dst->elem->is_const)) {
            return false;
        }
        if (dst->elem && dst->elem->kind == TypeKind::Void) return true;
        return same_type(src->elem, dst->elem);
    }

    // 数组 -> 切片
    if (src->kind == TypeKind::Array && dst->kind == TypeKind::Slice) {
        return same_type(src->elem, dst->elem);
    }

    // 切片：T[] -> const T[]；反过来必须显式 as
    if (src->kind == TypeKind::Slice && dst->kind == TypeKind::Slice) {
        if (!same_type(src->elem, dst->elem)) return false;
        if (src->elem && src->elem->is_const && !(dst->elem && dst->elem->is_const)) {
            return false;
        }
        return true;
    }

    // string -> const char* / const char[] | const byte[]
    if (src->kind == TypeKind::String) {
        if (dst->kind == TypeKind::Ptr && dst->elem && dst->elem->is_const) {
            if (dst->elem->kind == TypeKind::Void) return true;
            return dst->elem->kind == TypeKind::Prim && dst->elem->name == "char";
        }
        if (dst->kind == TypeKind::Slice && dst->elem && dst->elem->is_const) {
            if (dst->elem->kind != TypeKind::Prim) return false;
            return dst->elem->name == "char" || dst->elem->name == "byte";
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

    // 枚举是名义类型：与整数之间没有隐式转换，双向都必须显式 `as`。
    // （见 SPEC §3.5；判定放在这里意味着参数/返回值/赋值/字面量适配都不接受枚举）

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
    // 枚举不参与算术/位运算（同类型比较除外，见 sema 的 check_binary）。
    if (a_in->kind == TypeKind::Enum || b_in->kind == TypeKind::Enum) return nullptr;
    TypePtr a = a_in;
    TypePtr b = b_in;
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
