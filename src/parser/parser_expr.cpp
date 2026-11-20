// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Parsing — expressions
//
// owns:      the expression parser: precedence climbing, unary/postfix forms, calls, lambdas
// not:       type syntax (parser_type.cpp), declarations, statements, any semantic rule
// pass:      parsing
// concept:   parser-expr
#include "ast_clone.hpp"  // clone_spec: a shared type syntax must not share its array-length expression
#include "parser_impl.hpp"

#include <memory>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>

using namespace muc_detail;

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
    NestGuard guard(*this, nest_expr_, kMaxExprNest, kMaxParenNesting, "expression");
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
    int links = 0;
    while (at_kw("or")) {
        note_link(links, "expression");
        const Token tok = advance();
        ExprPtr right = parse_and();
        left = make_binary("or", std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_and() {
    ExprPtr left = parse_not();
    int links = 0;
    while (at_kw("and")) {
        note_link(links, "expression");
        const Token tok = advance();
        ExprPtr right = parse_not();
        left = make_binary("and", std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_not() {
    NestGuard guard(*this, nest_expr_, kMaxExprNest, kMaxParenNesting, "expression");
    // `move x` binds like a prefix operator, but the operand is restricted to a name (sema checks it):
    // partial moves are not allowed (agents/value-semantics.md §8.4).
    if (at_kw("move")) {
        const Token tok = advance();
        auto node = std::make_unique<MoveExpr>();
        node->operand = parse_unary();
        node->line = tok.line;
        node->col = tok.col;
        return node;
    }
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
    int links = 0;
    while (at_op("==", "!=")) {
        note_link(links, "expression");
        const Token tok = advance();
        ExprPtr right = parse_rel();
        left = make_binary(tok.text, std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_rel() {
    ExprPtr left = parse_bitor();
    int links = 0;
    for (;;) {
        const bool cmp = at_op("<", "<=", ">", ">=");
        // `x is Alt(a, b)` / `x is Cat d`: `is` is a contextual keyword (an ordinary identifier
        // everywhere else, so `int is = 1;` keeps working). A type name after `is` may be a keyword
        // type (`x is int`), which reaches sema and is rejected there with a clear message.
        const bool is_op = peek().kind == TokKind::Ident && peek().text == "is" &&
                           (peek(1).kind == TokKind::Ident ||
                            (peek(1).kind == TokKind::Kw && prims().count(peek(1).text) != 0));
        // Only an actual link extends the chain; counting the terminating iteration rejected a chain of
        // exactly `kMaxChainLinks` operators (#105).
        if (!cmp && !is_op) break;
        note_link(links, "expression");
        if (cmp) {
            const Token tok = advance();
            ExprPtr right = parse_bitor();
            left = make_binary(tok.text, std::move(left), std::move(right), tok);
            continue;
        }
        if (is_op) {
            const Token tok = advance();
            auto node = std::make_unique<IsExpr>();
            node->obj = std::move(left);
            if (peek().kind == TokKind::Kw) {
                node->name = advance().text;  // a primitive type name: always a static test
            } else {
                node->name = expect_ident("alternative or type name").text;
            }
            if (at_op("(")) {
                advance();
                if (!at_op(")")) {
                    for (;;) {
                        node->binds.push_back(expect_ident("bound name").text);
                        if (at_op(",")) {
                            advance();
                            continue;
                        }
                        break;
                    }
                }
                expect_op(")");
            } else if (peek().kind == TokKind::Ident && !peek().text.empty() &&
                       peek().text != "and" && peek().text != "or" && peek().text != "else") {
                // class/interface test with a binding: `x is Cat d`
                node->binds.push_back(advance().text);
            }
            node->line = tok.line;
            node->col = tok.col;
            left = std::move(node);
            continue;
        }
        break;
    }
    return left;
}

ExprPtr Parser::parse_bitor() {
    ExprPtr left = parse_bitxor();
    int links = 0;
    while (at_op("|")) {
        note_link(links, "expression");
        const Token tok = advance();
        ExprPtr right = parse_bitxor();
        left = make_binary("|", std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_bitxor() {
    ExprPtr left = parse_bitand();
    int links = 0;
    while (at_op("^")) {
        note_link(links, "expression");
        const Token tok = advance();
        ExprPtr right = parse_bitand();
        left = make_binary("^", std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_bitand() {
    ExprPtr left = parse_shift();
    int links = 0;
    while (at_op("&")) {
        note_link(links, "expression");
        const Token tok = advance();
        ExprPtr right = parse_shift();
        left = make_binary("&", std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_shift() {
    ExprPtr left = parse_add();
    int links = 0;
    while (at_op("<<", ">>")) {
        note_link(links, "expression");
        const Token tok = advance();
        ExprPtr right = parse_add();
        left = make_binary(tok.text, std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_add() {
    ExprPtr left = parse_mul();
    int links = 0;
    while (at_op("+", "-")) {
        note_link(links, "expression");
        const Token tok = advance();
        ExprPtr right = parse_mul();
        left = make_binary(tok.text, std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_mul() {
    ExprPtr left = parse_unary();
    int links = 0;
    while (at_op("*", "/", "%")) {
        note_link(links, "expression");
        const Token tok = advance();
        ExprPtr right = parse_unary();
        left = make_binary(tok.text, std::move(left), std::move(right), tok);
    }
    return left;
}

ExprPtr Parser::parse_unary() {
    NestGuard guard(*this, nest_expr_, kMaxExprNest, kMaxParenNesting, "expression");
    if (at_kw("match")) return parse_match_expr();
    const Token t = peek();
    const bool uninit = at_kw("uninit");
    if (uninit) {
        advance();
        if (!at_kw("new")) throw err_at("`uninit` must be followed by `new`", peek());
    }
    if (at_kw("new")) {
        advance();
        auto ne = std::make_unique<NewExpr>();
        ne->uninit = uninit;
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
        if (uninit) throw err_at("`uninit` applies only to dynamic arrays (`uninit new T[n]`)", t);
        return parse_postfix_tail(std::move(ne));
    }
    if (at_op("!")) {
        throw err("`!` is no longer used: use `not` for logical negation and `~` for bitwise negation");
    }
    if (t.kind == TokKind::Op && at_op("-", "+", "&", "*", "~")) {
        advance();
        ExprPtr operand = parse_unary();
        // `-9223372036854775808`: the magnitude is exactly 2^63, which no signed 64-bit literal can
        // hold on its own. Only a directly applied unary `-` makes it the signed minimum.
        if (auto il = dynamic_cast<IntLit*>(operand.get()); il && il->min_magnitude) {
            if (t.text != "-") {
                throw err_at("integer literal is too large (maximum 2^63-1): a 2^63 magnitude is only "
                             "valid directly after unary `-`",
                             t);
            }
            il->min_magnitude = false;
            il->value = std::numeric_limits<long long>::min();
            il->line = t.line;
            il->col = t.col;
            return parse_postfix_tail(std::move(operand));
        }
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
            // A generic instantiation from another unit (`sizeof(IIterator<int>)`) is not in the parser's
            // name table, so probe the `Name<args>` / `(T) => R` shapes speculatively and backtrack.
            const bool probe_type_shape =
                (peek().kind == TokKind::Ident && peek(1).is_op("<")) ||
                (peek().kind == TokKind::Op && peek().text == "(");
            bool as_type = is_type_start(peek());
            if (!as_type && probe_type_shape) {
                const std::size_t save = pos_;
                try {
                    parse_type();
                    as_type = at_op(")");
                } catch (const MuError&) {
                    as_type = false;
                }
                pos_ = save;
            }
            if (as_type) {
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
    if (at_kw("swap")) {
        const Token sw = advance();
        expect_op("(");
        auto node = std::make_unique<SwapExpr>();
        node->a = parse_expr();
        expect_op(",");
        node->b = parse_expr();
        expect_op(")");
        node->line = sw.line;
        node->col = sw.col;
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
    int links = 0;
    for (;;) {
        const Token t = peek();
        // Count only an actual link, so a chain of exactly `kMaxChainLinks` links is accepted (#105).
        // `f<int>(...)`, and a qualified call `unit.f<int>(...)`: the unit prefix parses as a Name, so
        // the whole callee is a Member whose `<` must be tried as type arguments too.
        const bool type_arg_candidate = dynamic_cast<Name*>(e.get()) || dynamic_cast<Member*>(e.get());
        const bool starts_link =
            t.is_op("(") || t.is_op("[") || t.is_op("->") || t.is_op(".") || t.is_kw("as") ||
            (t.is_op("<") && pending_type_args.empty() && type_arg_candidate);
        if (!starts_link) break;
        note_link(links, "expression");
        // generic call: `name<...>(`. If the trial parse fails, fall back to a comparison expression.
        if (t.is_op("<") && pending_type_args.empty() && type_arg_candidate) {
            const std::size_t save = pos_;
            bool ok = false;
            try {
                std::vector<TypeSpecPtr> args = parse_type_args();
                // `f<int>(...)` or a generic sum alternative `Option<int>.Some(...)` / `Option<int>.None`:
                // the type arguments belong to the call or the member access. A qualified callee
                // (`unit.f<int>(...)`) only takes the call form: `a.b < c` must stay a comparison.
                const bool member_callee = dynamic_cast<Member*>(e.get()) != nullptr;
                if (at_op("(") || (!member_callee && at_op(".") && peek(1).kind == TokKind::Ident)) {
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
            // a generic sum's alternative: `Option<int>.None` / `Option<int>.Some(...)`
            if (!pending_type_args.empty() && dynamic_cast<Name*>(node->obj.get())) {
                node->type_args = std::move(pending_type_args);
                pending_type_args.clear();
            }
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
    // (A capture list `[...]` is consumed by parse_lambda before this check; the `[` case is handled by
    // the caller.)
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
    auto node = std::make_unique<Lambda>();
    const Token t0 = peek();
    node->line = t0.line;
    node->col = t0.col;
    // Capture list: `[x, ref y, weak z]` (U41). Explicit only: a name used inside the body but not
    // listed is an error, so the copying cost is visible at the definition site.
    if (at_op("[")) {
        advance();
        if (!at_op("]")) {
            for (;;) {
                Lambda::Capture cap;
                const Token ct = peek();
                cap.line = ct.line;
                cap.col = ct.col;
                if (at_kw("weak")) {
                    advance();
                    cap.by_weak = true;
                }
                // `[ref weak x]` (the same modifiers the other way round) reaches sema as a pair and is
                // rejected there with one clear message.
                if (at_kw("ref")) {
                    advance();
                    cap.by_ref = true;
                    if (at_kw("weak")) {
                        advance();
                        cap.by_weak = true;
                    }
                }
                if (at_kw("this")) {
                    advance();
                    cap.name = "this";
                } else {
                    cap.name = expect_ident("captured variable").text;
                }
                node->captures.push_back(std::move(cap));
                if (at_op(",")) {
                    advance();
                    continue;
                }
                break;
            }
        }
        expect_op("]");
    }
    const Token t = expect_op("(");
    node->line = t.line;
    node->col = t.col;
    if (!at_op(")")) {
        for (;;) {
            if (at_kw("ref")) throw err_at("a lambda cannot take a `ref` parameter (a borrow is not a value)", peek());
            // The same parameter cap as a named function (#100).
            if (node->params.size() >= kMaxParams) {
                throw err_at("a lambda cannot declare more than " + std::to_string(kMaxParams) +
                                 " parameters; take a struct instead",
                             peek());
            }
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
    // `int()` / `bool()` / `double()`: the zero value of a primitive type (SPEC 4). The type keyword
    // is not an expression anywhere else, so `T()` is the only spelling this form can be.
    if (t.kind == TokKind::Kw && is_prim_name(t.text) && peek(1).is_op("(") && peek(2).is_op(")")) {
        advance();  // type keyword
        advance();  // (
        advance();  // )
        auto spec = std::make_unique<TypeSpec>();
        spec->kind = TypeSpec::K::Named;
        spec->name = t.text;
        spec->line = t.line;
        spec->col = t.col;
        auto callee = std::make_unique<Name>();
        callee->ident = t.text;
        callee->line = t.line;
        callee->col = t.col;
        auto call = std::make_unique<Call>();
        call->callee = std::move(callee);
        call->zero_spec = std::move(spec);
        call->line = t.line;
        call->col = t.col;
        return parse_postfix_tail(std::move(call));
    }
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
        node->isuffix = t.isuffix;
        node->min_magnitude = t.int_min_mag;
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
    if (t.kind == TokKind::Op && t.text == "[") {
        // A capture list starts a lambda only when a parameter list and `=>` follow it.
        int depth = 0;
        for (std::size_t i = pos_; i < toks_.size(); ++i) {
            const Token& tk = toks_[i];
            if (tk.kind != TokKind::Op) continue;
            if (tk.text == "[") {
                depth++;
            } else if (tk.text == "]") {
                depth--;
                if (depth == 0) {
                    if (i + 1 < toks_.size() && toks_[i + 1].kind == TokKind::Op && toks_[i + 1].text == "(") {
                        const std::size_t saved = pos_;
                        pos_ = i + 1;
                        const bool is_lambda = looks_like_lambda();
                        pos_ = saved;
                        if (is_lambda) return parse_lambda();
                    }
                    break;
                }
            } else if (tk.text == ";" || tk.text == "{" || tk.text == "}") {
                break;
            }
        }
        throw err_at("expected an expression", t);
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
