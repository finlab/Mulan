// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Parsing — types and expressions
#include "parser_impl.hpp"

#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

using namespace muc_detail;

namespace muc_detail {

// matches mtypes.PRIM_NAMES (PRIMS in parser.py); `string` is now the core.Str struct
static const std::unordered_set<std::string>& prims() {
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
    bool base_const = false;
    if (at_kw("const")) {
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
    } else if (t.kind == TokKind::Op && t.text == "(") {
        // function type `(T1, T2) => R` (3.16). Suffixes bind to the return type, so the recursive call consumes
        // them and this branch returns without running type_suffixes on the function type itself.
        if (base_const) {
            throw err_at("`const` cannot qualify an anonymous function type; name it with `type F = ...` and write "
                         "`const F` for a read-only binding",
                         t);
        }
        advance();
        auto fn = std::make_unique<TypeSpec>();
        fn->kind = TypeSpec::K::Func;
        fn->line = t.line;
        fn->col = t.col;
        if (!at_op(")")) {
            for (;;) {
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
    if (at_kw("const")) {
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
    while (at_op("*")) {
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
    for (;;) {
        if (at_op("*")) {
            const Token tok = advance();
            auto ptr = std::make_unique<TypeSpec>();
            ptr->kind = TypeSpec::K::Ptr;
            ptr->elem = std::move(spec);
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
            std::vector<size_t> dims;
            for (;;) {
                const Token n = peek();
                if (n.kind != TokKind::Int) throw err("array length only supports integer literals for now");
                advance();
                if (n.ival <= 0) throw err_at("array length must be greater than 0", n);
                dims.push_back(static_cast<size_t>(n.ival));
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
                arr->size = *it;
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

// ---------------------------------------------------------------- expressions

ExprPtr Parser::make_binary(const std::string& op, ExprPtr left, ExprPtr right,
                            const Token& tok) {
    auto node = std::make_unique<Binary>();
    node->op = op;
    node->left = std::move(left);
    node->right = std::move(right);
    node->line = tok.line;
    node->col = tok.col;
    return node;
}

ExprPtr Parser::parse_ternary() {
    ExprPtr cond = parse_or();
    if (!at_op("?")) return cond;
    const Token tok = advance();
    auto node = std::make_unique<Ternary>();
    node->cond = std::move(cond);
    node->then_e = parse_ternary();  // the middle branch may nest `?:`
    if (!at_op(":")) throw err_at("expected `:` after `?`", peek());
    advance();
    node->else_e = parse_ternary();  // right-associative: `a ? b : c ? d : e` = `a ? b : (c ? d : e)`
    node->line = tok.line;
    node->col = tok.col;
    return node;
}

ExprPtr Parser::parse_or() {
    ExprPtr left = parse_and();
    while (at_kw("or")) {
        const Token tok = advance();
        ExprPtr right = parse_and();
        left = make_binary("or", std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_and() {
    ExprPtr left = parse_not();
    while (at_kw("and")) {
        const Token tok = advance();
        ExprPtr right = parse_not();
        left = make_binary("and", std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_not() {
    if (at_kw("not")) {
        const Token tok = advance();
        ExprPtr operand = parse_not();
        auto node = std::make_unique<NotExpr>();
        node->operand = std::move(operand);
        node->line = tok.line;
        node->col = tok.col;
        return node;
    }
    return parse_eq();
}

ExprPtr Parser::parse_eq() {
    ExprPtr left = parse_rel();
    while (at_op("==", "!=")) {
        const Token tok = advance();
        ExprPtr right = parse_rel();
        left = make_binary(tok.text, std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_rel() {
    ExprPtr left = parse_bitor();
    while (at_op("<", "<=", ">", ">=")) {
        const Token tok = advance();
        ExprPtr right = parse_bitor();
        left = make_binary(tok.text, std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_bitor() {
    ExprPtr left = parse_bitxor();
    while (at_op("|")) {
        const Token tok = advance();
        ExprPtr right = parse_bitxor();
        left = make_binary("|", std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_bitxor() {
    ExprPtr left = parse_bitand();
    while (at_op("^")) {
        const Token tok = advance();
        ExprPtr right = parse_bitand();
        left = make_binary("^", std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_bitand() {
    ExprPtr left = parse_shift();
    while (at_op("&")) {
        const Token tok = advance();
        ExprPtr right = parse_shift();
        left = make_binary("&", std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_shift() {
    ExprPtr left = parse_add();
    while (at_op("<<", ">>")) {
        const Token tok = advance();
        ExprPtr right = parse_add();
        left = make_binary(tok.text, std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_add() {
    ExprPtr left = parse_mul();
    while (at_op("+", "-")) {
        const Token tok = advance();
        ExprPtr right = parse_mul();
        left = make_binary(tok.text, std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_mul() {
    ExprPtr left = parse_unary();
    while (at_op("*", "/", "%")) {
        const Token tok = advance();
        ExprPtr right = parse_unary();
        left = make_binary(tok.text, std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_unary() {
    const Token t = peek();
    if (at_kw("new")) {
        advance();
        auto ne = std::make_unique<NewExpr>();
        ne->type = parse_type_base();
        ne->line = t.line;
        ne->col = t.col;
        if (at_op("[")) {
            const Token lb = advance();
            if (at_op("]")) throw err_at("`new` dynamic array requires a length", lb);
            for (;;) {
                ne->dims.push_back(parse_expr());
                if (at_op(",")) {
                    advance();
                    continue;
                }
                break;
            }
            expect_op("]");
            // new T[m][]: each element of the outer array is a T[] (empty slot, rows are newed later)
            while (at_op("[")) {
                if (!peek(1).is_op("]")) {
                    throw err_at("`new T[m][n]` is not jagged array syntax: use `new T[m,n]` for rectangular arrays; for jagged arrays, allocate the outer array first and fill the rows in a loop", peek());
                }
                advance();
                advance();
                ne->jagged_rank++;
            }
            ne->is_array = true;
            if (at_op("(")) throw err_at("dynamic array construction does not support constructor arguments", lb);
            return parse_postfix_tail(std::move(ne));
        }
        if (at_op("(")) {
            advance();
            while (!at_op(")")) {
                if (peek().kind == TokKind::Ident && peek(1).is_op("=")) {
                    std::string nm = advance().text;
                    advance();
                    ne->named.emplace_back(std::move(nm), parse_expr());
                } else {
                    ne->args.push_back(parse_expr());
                    forbid_inc_expr();
                }
                if (at_op(",")) {
                    advance();
                } else {
                    break;
                }
            }
            expect_op(")");
            if (!ne->args.empty() && !ne->named.empty())
                throw err_at("positional and named arguments cannot be mixed", t);
        }
        return parse_postfix_tail(std::move(ne));
    }
    if (at_op("!")) {
        throw err("`!` is no longer used: use `not` for logical negation and `~` for bitwise negation");
    }
    if (t.kind == TokKind::Op && at_op("-", "+", "&", "*", "~")) {
        advance();
        ExprPtr operand = parse_unary();
        auto node = std::make_unique<Unary>();
        node->op = t.text;
        node->operand = std::move(operand);
        node->line = t.line;
        node->col = t.col;
        return parse_postfix_tail(std::move(node));
    }
    if (at_kw("sizeof", "alignof")) {
        std::string which = advance().text;
        auto node = std::make_unique<SizeofExpr>();
        node->which = std::move(which);
        if (at_op("(")) {
            advance();
            // `sizeof(T)` vs `sizeof(expr)`: a type starts with a type keyword, a known type name or an
            // imported unit prefix; anything else is an expression (`sizeof(a[0])`, `sizeof(p->x)`).
            if (is_type_start(peek())) {
                node->spec = parse_type();
            } else {
                node->expr = parse_expr();
            }
            expect_op(")");
        } else {
            // C's paren-free form: `sizeof *p`, `sizeof a`, `sizeof a[i]`; a trailing `as` applies to the
            // result, not to the operand
            const bool saved = suppress_as_;
            suppress_as_ = true;
            node->expr = parse_unary();
            suppress_as_ = saved;
        }
        node->line = t.line;
        node->col = t.col;
        return parse_postfix_tail(std::move(node));
    }
    if (at_kw("offsetof")) {
        advance();
        expect_op("(");
        auto node = std::make_unique<SizeofExpr>();
        node->which = "offsetof";
        node->spec = parse_type();
        expect_op(",");
        node->field = expect_ident("field name").text;
        expect_op(")");
        node->line = t.line;
        node->col = t.col;
        return parse_postfix_tail(std::move(node));
    }
    if (at_kw("len")) {
        throw err("`len` was removed: use `x.length()` for a managed sequence, or "
                  "`sizeof(a) / sizeof(a[0])` for a static array");
    }
    return parse_postfix();
}

ExprPtr Parser::parse_postfix() {
    return parse_postfix_tail(parse_primary());
}

ExprPtr Parser::parse_postfix_tail(ExprPtr e) {
    std::vector<TypeSpecPtr> pending_type_args;  // type arguments of `f<int>(...)`
    for (;;) {
        const Token t = peek();
        // generic call: `name<...>(`. If the trial parse fails, fall back to a comparison expression.
        if (t.is_op("<") && pending_type_args.empty() && dynamic_cast<Name*>(e.get())) {
            const std::size_t save = pos_;
            bool ok = false;
            try {
                std::vector<TypeSpecPtr> args = parse_type_args();
                if (at_op("(")) {
                    pending_type_args = std::move(args);
                    ok = true;
                }
            } catch (const MuError&) {
                ok = false;
            }
            if (!ok) pos_ = save;
            if (ok) continue;
        }
        if (t.is_op("(")) {
            e = parse_call(std::move(e));
            if (!pending_type_args.empty()) {
                static_cast<Call*>(e.get())->type_args = std::move(pending_type_args);
                pending_type_args.clear();
            }
            continue;
        }
        if (t.is_op("[")) {
            advance();
            auto node = std::make_unique<Index>();
            for (;;) {
                node->indices.push_back(parse_expr());
                forbid_inc_expr();
                if (at_op(",")) {
                    advance();
                    continue;
                }
                break;
            }
            expect_op("]");
            node->base = std::move(e);
            node->line = t.line;
            node->col = t.col;
            e = std::move(node);
            continue;
        }
        if (t.is_op("->")) {
            // `p->f` is exactly `(*p).f`: the postfix chain after a one-level dereference. Keeping it
            // parser sugar means sema/codegen have no second code path to keep in sync.
            advance();
            const Token nameTok = peek();
            std::string nm = expect_ident("member name").text;
            auto deref = std::make_unique<Unary>();
            deref->op = "*";
            deref->operand = std::move(e);
            deref->line = t.line;
            deref->col = t.col;
            auto node = std::make_unique<Member>();
            node->obj = std::move(deref);
            node->name = std::move(nm);
            node->line = nameTok.line;
            node->col = nameTok.col;
            e = std::move(node);
            continue;
        }
        if (t.is_op(".")) {
            // multi-segment unit name: in `lib.a.b.f`, `lib.a.b` is the unit; merge them into a single Name
            // so it is not split into three nested member accesses. Match by longest imported prefix.
            if (auto* n = dynamic_cast<Name*>(e.get());
                n && peek(1).kind == TokKind::Ident) {
                const std::string cand = n->ident + "." + peek(1).text;
                if (module_names_.count(cand)) {
                    advance();  // '.'
                    advance();  // member name
                    n->ident = cand;
                    continue;
                }
            }
            advance();
            std::string nm = expect_ident("member name").text;
            auto node = std::make_unique<Member>();
            node->obj = std::move(e);
            node->name = std::move(nm);
            node->line = t.line;
            node->col = t.col;
            e = std::move(node);
            continue;
        }
        if (t.is_kw("as")) {
            if (suppress_as_) break;  // the `as` belongs to the enclosing `sizeof` result
            advance();
            TypeSpecPtr spec = parse_type();
            auto node = std::make_unique<Cast>();
            node->expr = std::move(e);
            node->spec = std::move(spec);
            node->line = t.line;
            node->col = t.col;
            e = std::move(node);
            continue;
        }
        break;
    }
    return e;
}

ExprPtr Parser::parse_call(ExprPtr callee) {
    const Token t = expect_op("(");
    std::vector<ExprPtr> args;
    std::vector<std::pair<std::string, ExprPtr>> named;
    while (!at_op(")")) {
        if (peek().kind == TokKind::Ident && peek(1).is_op("=")) {
            std::string nm = advance().text;
            advance();
            named.emplace_back(std::move(nm), parse_expr());
        } else {
            args.push_back(parse_expr());
            forbid_inc_expr();
        }
        if (at_op(",")) {
            advance();
        } else {
            break;
        }
    }
    expect_op(")");
    if (!args.empty() && !named.empty()) throw err_at("positional and named arguments cannot be mixed", t);
    auto node = std::make_unique<Call>();
    node->callee = std::move(callee);
    node->args = std::move(args);
    node->named = std::move(named);
    node->line = t.line;
    node->col = t.col;
    return node;
}

bool Parser::looks_like_lambda() const {
    // pos_ points at `(`; find the matching `)` (skipping nested parentheses) and check for a following `=>`.
    int depth = 0;
    for (std::size_t i = pos_; i < toks_.size(); ++i) {
        const Token& tk = toks_[i];
        if (tk.kind != TokKind::Op) continue;
        if (tk.text == "(") {
            depth++;
        } else if (tk.text == ")") {
            depth--;
            if (depth == 0) {
                return i + 1 < toks_.size() && toks_[i + 1].kind == TokKind::Op && toks_[i + 1].text == "=>";
            }
        } else if (tk.text == "{" || tk.text == "}" || tk.text == ";") {
            return false;
        }
    }
    return false;
}

ExprPtr Parser::parse_lambda() {
    const Token t = expect_op("(");
    auto node = std::make_unique<Lambda>();
    node->line = t.line;
    node->col = t.col;
    if (!at_op(")")) {
        for (;;) {
            if (at_kw("ref")) throw err_at("a lambda cannot take a `ref` parameter (a borrow is not a value)", peek());
            auto p = std::make_unique<Param>();
            const Token ptok = peek();
            p->spec = parse_type();
            if (at_op(")")) {
                throw err_at("a lambda parameter must be written as `type name`; an anonymous function type cannot "
                             "start a declaration (write `auto name = ...` or name the type with `type`)",
                             ptok);
            }
            p->name = expect_ident("parameter name").text;
            p->line = ptok.line;
            p->col = ptok.col;
            node->params.push_back(std::move(p));
            if (at_op(",")) {
                advance();
                continue;
            }
            break;
        }
    }
    expect_op(")");
    expect_op("=>");
    if (at_op("{")) {
        node->block_body = parse_block();
    } else {
        node->expr_body = parse_expr();
        forbid_inc_expr();
    }
    return node;
}

ExprPtr Parser::parse_primary() {
    const Token t = peek();
    if (t.kind == TokKind::Kw && (t.text == "this" || t.text == "super")) {
        advance();
        auto node = std::make_unique<Name>();
        node->ident = t.text;
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Int) {
        advance();
        auto node = std::make_unique<IntLit>();
        node->value = t.ival;
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Float) {
        advance();
        auto node = std::make_unique<FloatLit>();
        node->value = t.fval;
        node->is_float32 = t.f32;
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Char) {
        advance();
        auto node = std::make_unique<CharLit>();
        node->value = static_cast<int>(t.ival);
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Str) {
        advance();
        auto node = std::make_unique<StringLit>();
        node->value = t.bytes;
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Kw && (t.text == "true" || t.text == "false")) {
        advance();
        auto node = std::make_unique<BoolLit>();
        node->value = t.text == "true";
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Kw && t.text == "null") {
        advance();
        auto node = std::make_unique<NullLit>();
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Kw && (t.text == "alloc" || t.text == "realloc" || t.text == "free")) {
        advance();
        auto node = std::make_unique<Name>();
        node->ident = t.text;
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Ident) {
        advance();
        auto node = std::make_unique<Name>();
        node->ident = t.text;
        node->line = t.line;
        node->col = t.col;
        return node;
    }
    if (t.kind == TokKind::Op && t.text == "(") {
        if (looks_like_lambda()) return parse_lambda();
        advance();
        ExprPtr e = parse_expr();
        if (at_op("++", "--")) {
            throw err("`++`/`--` has no value and cannot appear in an expression (only as a standalone statement)");
        }
        expect_op(")");
        return e;
    }
    throw err("unexpected " + t.describe());
}
