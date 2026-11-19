// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Parsing — type syntax (Parser member function definitions).
//
// owns:      `parse_type` and its parts: primitive / named / qualified names, generic argument and
//            parameter lists, `>>` splitting, and the array / slice / pointer / function suffixes
// not:       the expression parser (parser_expr.cpp), declarations, statements, any semantic rule
// pass:      parsing
// concept:   parser-types
#include "ast_clone.hpp"  // clone_spec: a shared type syntax must not share its array-length expression
#include "parser_impl.hpp"

#include <memory>
#include <string>
#include <unordered_set>

using namespace muc_detail;

namespace muc_detail {

const std::unordered_set<std::string>& Parser::prims() {
    static const std::unordered_set<std::string> s = {
        "byte",  "char",   "short", "ushort", "int",   "uint",  "long",
        "ulong", "usize", "isize", "float",  "double", "bool",  "void",
    };
    return s;
}

// Multiple declarators in one declaration share a single type syntax; deep-copy so each owns its own TypeSpec.
TypeSpecPtr clone_spec(const TypeSpec& s) {
    auto out = std::make_unique<TypeSpec>();
    out->kind = s.kind;
    out->name = s.name;
    out->args.reserve(s.args.size());
    for (const auto& a : s.args) out->args.push_back(a ? clone_spec(*a) : nullptr);
    out->elem = s.elem ? clone_spec(*s.elem) : nullptr;
    out->params.reserve(s.params.size());
    for (const auto& p : s.params) out->params.push_back(p ? clone_spec(*p) : nullptr);
    out->ret = s.ret ? clone_spec(*s.ret) : nullptr;
    out->size = s.size;
    out->len = s.len ? clone_expr(*s.len) : nullptr;
    out->rank = s.rank;
    out->is_const = s.is_const;
    out->is_self = s.is_self;
    out->weak = s.weak;
    out->line = s.line;
    out->col = s.col;
    return out;
}

static TypeSpecPtr make_named_spec(const std::string& name, int line, int col) {
    auto spec = std::make_unique<TypeSpec>();
    spec->kind = TypeSpec::K::Named;
    spec->name = name;
    spec->line = line;
    spec->col = col;
    return spec;
}

}  // namespace muc_detail

// ---------------------------------------------------------------- types

bool Parser::is_type_start(const Token& t) const {
    if (t.kind == TokKind::Kw) {
        if (prims().count(t.text) != 0) return true;
        if (t.text == "const") return true;
        return false;
    }
    if (t.kind == TokKind::Ident) {
        // anonymous function / delegate type at the start of a type (`delegate (int) => int d;` is still
        // not allowed in declaration position, but `sizeof(delegate (int) => int)` and casts see it).
        if (t.text == "delegate" && peek(1).is_op("(")) return true;
        if (type_names_.count(t.text) != 0) return true;
        // qualified type `math.Point` / `std.string.string` / `lib.a.b.T`
        if (peek(1).is_op(".") && is_module_prefix(t.text)) return true;
    }
    return false;
}

// Whether this is the dotted prefix of an imported unit name (or std.string).
bool Parser::is_module_prefix(const std::string& s) const {
    for (const auto& m : module_names_) {
        if (m == s) return true;
        if (m.size() > s.size() && m.compare(0, s.size(), s) == 0 && m[s.size()] == '.') {
            return true;
        }
    }
    return false;
}

// generic argument list: `<int, Vec<long>>` (triggered by `name <` in `parse_type`)
std::vector<std::unique_ptr<TypeSpec>> Parser::parse_type_args() {
    std::vector<std::unique_ptr<TypeSpec>> args;
    expect_op("<");
    if (at_op(">")) throw err("type argument list cannot be empty");
    for (;;) {
        args.push_back(parse_type());
        if (at_op(",")) {
            advance();
            continue;
        }
        break;
    }
    expect_gt();
    return args;
}

// generic parameter list at a declaration: `<T>` / `<K, V>` / `<T: IList<int> & IComparable>`
std::vector<TypeParam> Parser::parse_type_params() {
    std::vector<TypeParam> out;
    if (!at_op("<")) return out;
    advance();
    if (at_op(">")) throw err("generic parameter list cannot be empty");
    for (;;) {
        const Token nt = expect_ident("type parameter name");
        TypeParam p;
        p.name = nt.text;
        p.line = nt.line;
        p.col = nt.col;
        if (at_op(":")) {
            advance();
            for (;;) {
                const Token bt = peek();
                TypeSpecPtr b;
                if (bt.kind == TokKind::Kw && prims().count(bt.text) != 0) {
                    advance();
                    b = make_named_spec(bt.text, bt.line, bt.col);
                } else if (bt.kind == TokKind::Ident) {
                    advance();
                    std::string qn = bt.text;
                    while (at_op(".")) {
                        advance();
                        qn += ".";
                        qn += expect_ident("type name").text;
                    }
                    b = make_named_spec(qn, bt.line, bt.col);
                } else {
                    throw err("expected constraint type but got " + bt.describe());
                }
                if (at_op("<")) b->args = parse_type_args();
                p.bounds.push_back(std::move(b));
                if (at_op("&") || at_op("|")) {
                    const TypeParam::Op next_op = at_op("&") ? TypeParam::Op::And : TypeParam::Op::Or;
                    if (p.op != TypeParam::Op::None && p.op != next_op) {
                        throw err("`&` and `|` cannot be mixed in one constraint list; "
                                  "split it into separate type parameters");
                    }
                    p.op = next_op;
                    advance();
                    continue;
                }
                break;
            }
        }
        out.push_back(std::move(p));
        if (at_op(",")) {
            advance();
            continue;
        }
        break;
    }
    expect_gt();
    return out;
}

// right angle bracket of type arguments: split a lexical `>>` into two `>` (`Vec<Vec<int>>`)
Token Parser::expect_gt() {
    if (at_op(">>")) {
        Token split = toks_[pos_];
        split.text = ">";
        toks_[pos_].text = ">";
        using Diff = std::vector<Token>::difference_type;
        toks_.insert(toks_.begin() + static_cast<Diff>(pos_) + 1, split);
        return advance();
    }
    if (!at_op(">")) throw err("expected `>` but got " + peek().describe());
    return advance();
}

TypeSpecPtr Parser::parse_type() {
    // Every recursive type position comes through here (generic arguments, array elements, pointers,
    // function parameter/result types), so this is where the nesting budget is spent; the guard used to
    // sit in `parse_type_base`, which the `Name<...>` path does not take (#102).
    // A declaration's outermost type spends one level before its first nested operand, so the budget is
    // one more than the documented count of nested type operands.
    NestGuard guard(*this, nest_type_, kMaxTypeNest + 1, kMaxTypeNest, "type");
    bool base_const = false;
    // Repeated `const` folds (`const const int` ≡ `const int`, SPEC 3.7).
    while (at_kw("const")) {
        advance();
        base_const = true;
    }
    const Token t = peek();
    TypeSpecPtr spec;
    if (t.kind == TokKind::Kw && prims().count(t.text) != 0) {
        advance();
        spec = make_named_spec(t.text, t.line, t.col);
    } else if (t.kind == TokKind::Ident && t.text == "delegate" && peek(1).is_op("(")) {
        // `delegate (T1, T2) => R` — the capturing counterpart of `(T1, T2) => R` ({code, env}, two
        // words). Handled before the generic name branch because `delegate` is an ordinary identifier
        // (a contextual keyword) and only a following `(` makes it a type.
        advance();  // `delegate`
        advance();  // `(`
        auto fn = std::make_unique<TypeSpec>();
        fn->kind = TypeSpec::K::Delegate;
        fn->line = t.line;
        fn->col = t.col;
        if (!at_op(")")) {
            for (;;) {
                if (fn->params.size() >= kMaxParams) {
                    throw err_at("a function type cannot declare more than " +
                                     std::to_string(kMaxParams) +
                                     " parameters; take a struct instead",
                                 peek());
                }
                fn->params.push_back(parse_type());
                if (at_op(",")) {
                    advance();
                    continue;
                }
                break;
            }
        }
        expect_op(")");
        if (!at_op("=>")) {
            throw err_at("expected `=>` after the parameter types of a delegate type", peek());
        }
        advance();
        fn->ret = parse_type();
        return fn;
    } else if (t.kind == TokKind::Ident) {
        advance();
        std::string name = t.text;
        while (at_op(".")) {
            advance();
            name += ".";
            name += expect_ident("type name").text;
        }
        spec = make_named_spec(name, t.line, t.col);
    } else if (t.kind == TokKind::Op && t.text == "(") {
        // function type `(T1, T2) => R` (3.16). Suffixes bind to the return type, so the recursive
        // call consumes them and this branch returns without running type_suffixes on the function
        // type itself.
        if (base_const) {
            throw err_at("`const` cannot qualify an anonymous function type; name it with `type F = ...` and write "
                         "`const F` for a read-only binding",
                         t);
        }
        advance();  // `(`
        auto fn = std::make_unique<TypeSpec>();
        fn->kind = TypeSpec::K::Func;
        fn->line = t.line;
        fn->col = t.col;
        if (!at_op(")")) {
            for (;;) {
                // The same parameter cap as a named function and a lambda (#100).
                if (fn->params.size() >= kMaxParams) {
                    throw err_at("a function type cannot declare more than " +
                                     std::to_string(kMaxParams) +
                                     " parameters; take a struct instead",
                                 peek());
                }
                fn->params.push_back(parse_type());
                if (at_op(",")) {
                    advance();
                    continue;
                }
                break;
            }
        }
        expect_op(")");
        if (!at_op("=>")) {
            throw err_at("expected `=>` after the parameter types of a function type", peek());
        }
        advance();
        fn->ret = parse_type();
        return fn;
    } else {
        throw err("expected type name but got " + t.describe());
    }
    if (at_op("<")) spec->args = parse_type_args();
    if (spec->kind == TypeSpec::K::Named && spec->name == "Self") spec->is_self = true;
    if (base_const) spec->is_const = true;
    return type_suffixes(std::move(spec), t);
}

TypeSpecPtr Parser::parse_type_base() {
    bool base_const = false;
    // Repeated `const` is allowed and folds (`const const int` ≡ `const int`, SPEC 3.7): a code
    // generator or a macro expansion may repeat a qualifier.
    while (at_kw("const")) {
        advance();
        base_const = true;
    }
    const Token t = peek();
    TypeSpecPtr spec;
    if (t.kind == TokKind::Kw && prims().count(t.text) != 0) {
        advance();
        spec = make_named_spec(t.text, t.line, t.col);
    } else if (t.kind == TokKind::Ident) {
        advance();
        std::string name = t.text;
        while (at_op(".")) {
            advance();
            name += ".";
            name += expect_ident("type name").text;
        }
        spec = make_named_spec(name, t.line, t.col);
    } else {
        throw err("expected type name but got " + t.describe());
    }
    if (at_op("<")) spec->args = parse_type_args();
    if (base_const) spec->is_const = true;
    int links = 0;
    while (at_op("*")) {
        note_link(links, "type", kMaxTypeLinks);
        const Token tok = advance();
        auto ptr = std::make_unique<TypeSpec>();
        ptr->kind = TypeSpec::K::Ptr;
        ptr->elem = std::move(spec);
        ptr->line = tok.line;
        ptr->col = tok.col;
        spec = std::move(ptr);
    }
    return spec;
}

TypeSpecPtr Parser::type_suffixes(TypeSpecPtr spec, const Token& t) {
    int links = 0;
    for (;;) {
        // Count only a suffix that is actually there: counting the terminating iteration rejected a
        // declaration with exactly the documented number of declarators (#99).
        if (!at_op("*") && !at_op("[")) break;
        note_link(links, "type", kMaxTypeLinks);
        if (at_op("*")) {
            const Token tok = advance();
            auto ptr = std::make_unique<TypeSpec>();
            ptr->kind = TypeSpec::K::Ptr;
            ptr->elem = std::move(spec);
            // `T* const`: the pointer itself is read-only (the pointee const is carried by the elem
            // spec). A repeated `const` folds (SPEC 3.7).
            while (at_kw("const")) {
                advance();
                ptr->is_const = true;
            }
            ptr->line = tok.line;
            ptr->col = tok.col;
            spec = std::move(ptr);
            continue;
        }
        if (at_op("[")) {
            const Token tok = advance();
            if (at_op("]")) {
                advance();
                auto slice = std::make_unique<TypeSpec>();
                slice->kind = TypeSpec::K::Slice;
                slice->elem = std::move(spec);
                slice->rank = 1;
                slice->line = tok.line;
                slice->col = tok.col;
                spec = std::move(slice);
                continue;
            }
            if (at_op(",")) {
                int rank = 1;
                while (at_op(",")) {
                    advance();
                    rank++;
                }
                expect_op("]");
                auto slice = std::make_unique<TypeSpec>();
                slice->kind = TypeSpec::K::Slice;
                slice->elem = std::move(spec);
                slice->rank = rank;
                slice->line = tok.line;
                slice->col = tok.col;
                spec = std::move(slice);
                continue;
            }
            std::vector<std::pair<size_t, ExprPtr>> dims;
            for (;;) {
                const Token n = peek();
                if (n.kind == TokKind::Int) {
                    advance();
                    if (n.int_min_mag) {
                        throw err_at("integer literal is too large (maximum 2^63-1): a 2^63 magnitude "
                                     "is only valid directly after unary `-`",
                                     n);
                    }
                    if (n.ival <= 0) throw err_at("array length must be greater than 0", n);
                    dims.emplace_back(static_cast<size_t>(n.ival), nullptr);
                } else {
                    // A compile-time constant length (`int[SIZE]`, `int[SIZE * 2]`): the semantic pass
                    // folds it against the unit's constants (SPEC 3.3.2). A local of the same name is
                    // not a compile-time value and is rejected there.
                    ExprPtr e = parse_expr();
                    if (!e) throw err_at("expected an array length", n);
                    dims.emplace_back(0, std::move(e));
                }
                if (at_op(",")) {
                    advance();
                    continue;
                }
                break;
            }
            expect_op("]");
            // wrap in reverse: int[2,3] -> Array(Array(int,3),2)
            for (auto it = dims.rbegin(); it != dims.rend(); ++it) {
                auto arr = std::make_unique<TypeSpec>();
                arr->kind = TypeSpec::K::Array;
                arr->elem = std::move(spec);
                arr->size = it->first;
                arr->len = std::move(it->second);
                arr->line = tok.line;
                arr->col = tok.col;
                spec = std::move(arr);
            }
            continue;
        }
        break;
    }

    return spec;
}
