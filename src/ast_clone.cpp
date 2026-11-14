// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// AST deep copy implementation (used for generic instantiation).
#include "ast_clone.hpp"

#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

namespace muc_detail {

namespace {

// Copies the position information (fields of the Node base class)
template <class T>
std::unique_ptr<T> mk(const Node& n) {
    auto r = std::make_unique<T>();
    r->line = n.line;
    r->col = n.col;
    return r;
}

// The name of a node kind, for the "unhandled node" diagnostic (`muc_detail::IsExpr`).
std::string node_kind(const Node& n) {
    std::string name = typeid(n).name();
    // MSVC prefixes the kind ("class muc_detail::IsExpr"); GCC does not.
    if (name.rfind("class ", 0) == 0) return name.substr(6);
    if (name.rfind("struct ", 0) == 0) return name.substr(7);
    return name;
}

// A node kind the clone table does not cover. Dropping it silently would change the meaning of the
// program (the statement disappears, the branch is lost), so the instantiation fails loudly.
[[noreturn]] void unsupported_kind(const std::string& kind, const char* what) {
    throw CloneError(std::string("internal: AST node kind not handled by the generic instantiator (") +
                       what + "): " + kind);
}

std::vector<std::unique_ptr<MethodDecl>> clone_methods(const std::vector<std::unique_ptr<MethodDecl>>& src);
std::vector<std::unique_ptr<CtorDecl>> clone_ctors(const std::vector<std::unique_ptr<CtorDecl>>& src);
std::vector<std::unique_ptr<ConstMember>> clone_const_members(
    const std::vector<std::unique_ptr<ConstMember>>& src);

std::vector<std::pair<std::string, TypeSpecPtr>> clone_fields(
    const std::vector<std::pair<std::string, TypeSpecPtr>>& src) {
    std::vector<std::pair<std::string, TypeSpecPtr>> out;
    out.reserve(src.size());
    for (const auto& f : src) {
        out.emplace_back(f.first, f.second ? clone_type_spec(*f.second) : nullptr);
    }
    return out;
}

std::vector<std::unique_ptr<Param>> clone_params(const std::vector<std::unique_ptr<Param>>& src) {
    std::vector<std::unique_ptr<Param>> out;
    out.reserve(src.size());
    for (const auto& p : src) out.push_back(clone_param(*p));
    return out;
}

std::vector<std::unique_ptr<MethodDecl>> clone_methods(
    const std::vector<std::unique_ptr<MethodDecl>>& src) {
    std::vector<std::unique_ptr<MethodDecl>> out;
    out.reserve(src.size());
    for (const auto& m : src) out.push_back(clone_method(*m));
    return out;
}

std::vector<std::unique_ptr<CtorDecl>> clone_ctors(const std::vector<std::unique_ptr<CtorDecl>>& src) {
    std::vector<std::unique_ptr<CtorDecl>> out;
    out.reserve(src.size());
    for (const auto& c : src) out.push_back(clone_ctor(*c));
    return out;
}

std::vector<std::unique_ptr<ConstMember>> clone_const_members(
    const std::vector<std::unique_ptr<ConstMember>>& src) {
    std::vector<std::unique_ptr<ConstMember>> out;
    out.reserve(src.size());
    for (const auto& c : src) out.push_back(clone_const_member(*c));
    return out;
}

std::vector<std::unique_ptr<TypeSpec>> clone_type_args(
    const std::vector<std::unique_ptr<TypeSpec>>& src) {
    std::vector<std::unique_ptr<TypeSpec>> out;
    out.reserve(src.size());
    for (const auto& a : src) out.push_back(a ? clone_type_spec(*a) : nullptr);
    return out;
}

std::vector<TypeParam> clone_type_params(const std::vector<TypeParam>& src) {
    std::vector<TypeParam> out;
    out.reserve(src.size());
    for (const auto& p : src) {
        TypeParam q;
        q.op = p.op;
        q.name = p.name;
        q.line = p.line;
        q.col = p.col;
        for (const auto& b : p.bounds) q.bounds.push_back(b ? clone_type_spec(*b) : nullptr);
        out.push_back(std::move(q));
    }
    return out;
}

// `for ... in ...`: also the loop of a `para for` statement.
std::unique_ptr<ForInStmt> clone_for_in(const ForInStmt& f) {
    auto r = mk<ForInStmt>(f);
    r->by_ref = f.by_ref;
    r->var = f.var;
    r->has_idx = f.has_idx;
    r->idx_name = f.idx_name;
    r->seq = clone_expr(*f.seq);
    r->step = f.step ? clone_expr(*f.step) : nullptr;
    r->body = clone_block(*f.body);
    return r;
}

}  // namespace

TypeSpecPtr clone_type_spec(const TypeSpec& s) {
    auto out = std::make_unique<TypeSpec>();
    out->kind = s.kind;
    out->name = s.name;
    out->args = clone_type_args(s.args);
    out->elem = s.elem ? clone_type_spec(*s.elem) : nullptr;
    out->params.reserve(s.params.size());
    for (const auto& p : s.params) out->params.push_back(p ? clone_type_spec(*p) : nullptr);
    out->ret = s.ret ? clone_type_spec(*s.ret) : nullptr;
    out->size = s.size;
    out->rank = s.rank;
    out->is_const = s.is_const;
    out->is_self = s.is_self;
    out->weak = s.weak;
    out->resolved = s.resolved;
    out->line = s.line;
    out->col = s.col;
    return out;
}

ExprPtr clone_expr(const Expr& e) {
    if (auto p = dynamic_cast<const IntLit*>(&e)) {
        auto r = mk<IntLit>(e);
        r->value = p->value;
        return r;
    }
    if (auto p = dynamic_cast<const FloatLit*>(&e)) {
        auto r = mk<FloatLit>(e);
        r->value = p->value;
        r->is_float32 = p->is_float32;
        return r;
    }
    if (auto p = dynamic_cast<const CharLit*>(&e)) {
        auto r = mk<CharLit>(e);
        r->value = p->value;
        return r;
    }
    if (auto p = dynamic_cast<const StringLit*>(&e)) {
        auto r = mk<StringLit>(e);
        r->value = p->value;
        return r;
    }
    if (auto p = dynamic_cast<const BoolLit*>(&e)) {
        auto r = mk<BoolLit>(e);
        r->value = p->value;
        return r;
    }
    if (dynamic_cast<const NullLit*>(&e)) {
        return mk<NullLit>(e);
    }
    if (auto p = dynamic_cast<const Name*>(&e)) {
        auto r = mk<Name>(e);
        r->ident = p->ident;  // kind / sym / field_index are refilled by sema
        return r;
    }
    if (auto p = dynamic_cast<const Member*>(&e)) {
        auto r = mk<Member>(e);
        r->obj = clone_expr(*p->obj);
        r->name = p->name;
        // the type arguments of a generic sum alternative (`Option<int>.None`)
        for (const auto& ta : p->type_args) r->type_args.push_back(ta ? clone_type_spec(*ta) : nullptr);
        return r;
    }
    if (auto p = dynamic_cast<const Index*>(&e)) {
        auto r = mk<Index>(e);
        r->base = clone_expr(*p->base);
        for (const auto& i : p->indices) r->indices.push_back(clone_expr(*i));
        return r;
    }
    if (auto p = dynamic_cast<const Unary*>(&e)) {
        auto r = mk<Unary>(e);
        r->op = p->op;
        r->operand = clone_expr(*p->operand);
        return r;
    }
    if (auto p = dynamic_cast<const NotExpr*>(&e)) {
        auto r = mk<NotExpr>(e);
        r->operand = clone_expr(*p->operand);
        return r;
    }
    if (auto p = dynamic_cast<const Binary*>(&e)) {
        auto r = mk<Binary>(e);
        r->op = p->op;
        r->left = clone_expr(*p->left);
        r->right = clone_expr(*p->right);
        return r;
    }
    if (auto p = dynamic_cast<const Cast*>(&e)) {
        auto r = mk<Cast>(e);
        r->expr = clone_expr(*p->expr);
        r->spec = p->spec ? clone_type_spec(*p->spec) : nullptr;
        return r;
    }
    if (auto p = dynamic_cast<const Ternary*>(&e)) {
        auto r = mk<Ternary>(e);
        r->cond = clone_expr(*p->cond);
        r->then_e = clone_expr(*p->then_e);
        r->else_e = clone_expr(*p->else_e);
        return r;
    }
    if (auto p = dynamic_cast<const Call*>(&e)) {
        auto r = mk<Call>(e);
        r->callee = clone_expr(*p->callee);
        for (const auto& a : p->args) r->args.push_back(clone_expr(*a));
        for (const auto& n : p->named) r->named.emplace_back(n.first, clone_expr(*n.second));
        for (const auto& ta : p->type_args) r->type_args.push_back(ta ? clone_type_spec(*ta) : nullptr);
        return r;
    }
    if (auto p = dynamic_cast<const NewExpr*>(&e)) {
        auto r = mk<NewExpr>(e);
        r->type = p->type ? clone_type_spec(*p->type) : nullptr;
        for (const auto& a : p->args) r->args.push_back(clone_expr(*a));
        for (const auto& n : p->named) r->named.emplace_back(n.first, clone_expr(*n.second));
        r->is_array = p->is_array;
        for (const auto& d : p->dims) r->dims.push_back(clone_expr(*d));
        r->jagged_rank = p->jagged_rank;
        return r;
    }
    if (auto p = dynamic_cast<const SizeofExpr*>(&e)) {
        auto r = mk<SizeofExpr>(e);
        r->spec = p->spec ? clone_type_spec(*p->spec) : nullptr;
        r->expr = p->expr ? clone_expr(*p->expr) : nullptr;
        r->field = p->field;
        r->which = p->which;
        return r;
    }
    if (auto p = dynamic_cast<const Len*>(&e)) {
        auto r = mk<Len>(e);
        r->seq = clone_expr(*p->seq);
        r->dim = p->dim ? clone_expr(*p->dim) : nullptr;
        return r;
    }
    if (auto p = dynamic_cast<const RangeExpr*>(&e)) {
        auto r = mk<RangeExpr>(e);
        r->lo = clone_expr(*p->lo);
        r->hi = clone_expr(*p->hi);
        r->inclusive = p->inclusive;
        return r;
    }
    if (auto p = dynamic_cast<const Lambda*>(&e)) {
        auto r = mk<Lambda>(e);
        for (const auto& cap : p->captures) r->captures.push_back(cap);
        for (const auto& prm : p->params) {
            if (prm) r->params.push_back(clone_param(*prm));
        }
        r->expr_body = p->expr_body ? clone_expr(*p->expr_body) : nullptr;
        r->block_body = p->block_body ? clone_block(*p->block_body) : nullptr;
        return r;
    }
    if (auto p = dynamic_cast<const IsExpr*>(&e)) {
        auto r = mk<IsExpr>(e);
        r->obj = clone_expr(*p->obj);
        r->name = p->name;
        r->binds = p->binds;
        // IsExpr shadows Node::line/col (the parser fills the shadowing ones), so copy them here
        r->line = p->line;
        r->col = p->col;
        // the sema fields (is_sum / sum_ty / alt / test_ty / bind_types) are derived again
        return r;
    }
    unsupported_kind(node_kind(e), "expression");
}

InitPtr clone_init(const Init& i) {
    if (auto p = dynamic_cast<const InitExpr*>(&i)) {
        auto r = mk<InitExpr>(i);
        r->kind = Init::K::Expr;
        r->expr = clone_expr(*p->expr);
        return r;
    }
    if (auto p = dynamic_cast<const InitAggregate*>(&i)) {
        auto r = mk<InitAggregate>(i);
        r->kind = Init::K::Aggregate;
        for (const auto& item : p->items) {
            r->items.emplace_back(item.first, item.second ? clone_init(*item.second) : nullptr);
        }
        return r;
    }
    if (auto p = dynamic_cast<const InitRange*>(&i)) {
        auto r = mk<InitRange>(i);
        r->kind = Init::K::Aggregate;  // consistent with the parser: range fill inside braces
        r->lo = clone_expr(*p->lo);
        r->hi = clone_expr(*p->hi);
        return r;
    }
    unsupported_kind(node_kind(i), "initializer");
}

std::unique_ptr<Block> clone_block(const Block& b) {
    auto r = mk<Block>(b);
    for (const auto& s : b.stmts) {
        auto c = clone_stmt(*s);
        // INVARIANT: clone_stmt never returns null (an unknown kind throws), so a dropped
        // statement can never be mistaken for a successful clone
        if (!c) unsupported_kind("<null>", "statement");
        r->stmts.push_back(std::move(c));
    }
    return r;
}

StmtPtr clone_stmt(const Stmt& s) {
    if (auto p = dynamic_cast<const Block*>(&s)) {
        return clone_block(*p);
    }
    if (auto p = dynamic_cast<const LocalVar*>(&s)) {
        auto r = mk<LocalVar>(s);
        r->name = p->name;
        r->spec = p->spec ? clone_type_spec(*p->spec) : nullptr;
        r->init = p->init ? clone_init(*p->init) : nullptr;
        r->is_const = p->is_const;
        r->is_auto = p->is_auto;
        r->by_ref = p->by_ref;
        r->is_ref_param = p->is_ref_param;
        r->group = p->group;
        return r;
    }
    if (auto p = dynamic_cast<const ExprStmt*>(&s)) {
        auto r = mk<ExprStmt>(s);
        r->expr = clone_expr(*p->expr);
        return r;
    }
    if (auto p = dynamic_cast<const AssignStmt*>(&s)) {
        auto r = mk<AssignStmt>(s);
        r->target = clone_expr(*p->target);
        r->op = p->op;
        r->value = clone_expr(*p->value);
        return r;
    }
    if (auto p = dynamic_cast<const IfStmt*>(&s)) {
        auto r = mk<IfStmt>(s);
        r->cond = clone_expr(*p->cond);
        r->then = clone_block(*p->then);
        r->els = p->els ? clone_stmt(*p->els) : nullptr;
        return r;
    }
    if (auto p = dynamic_cast<const WhileStmt*>(&s)) {
        auto r = mk<WhileStmt>(s);
        r->cond = clone_expr(*p->cond);
        r->body = clone_block(*p->body);
        return r;
    }
    if (auto p = dynamic_cast<const ForInStmt*>(&s)) {
        return clone_for_in(*p);
    }
    if (auto p = dynamic_cast<const ParaStmt*>(&s)) {
        auto r = mk<ParaStmt>(s);
        r->sections.reserve(p->sections.size());
        for (const auto& sec : p->sections) {
            if (!sec) unsupported_kind("<null>", "statement");
            r->sections.push_back(clone_block(*sec));
        }
        // the captures are a sema field: they are derived again from the cloned sections
        return r;
    }
    if (auto p = dynamic_cast<const ParaForStmt*>(&s)) {
        auto r = mk<ParaForStmt>(s);
        if (p->loop) r->loop = clone_for_in(*p->loop);
        // the captures are a sema field: they are derived again from the cloned loop
        return r;
    }
    if (auto p = dynamic_cast<const SwitchStmt*>(&s)) {
        auto r = mk<SwitchStmt>(s);
        r->scrut = clone_expr(*p->scrut);
        r->arms.reserve(p->arms.size());
        for (const auto& a : p->arms) {
            auto arm = std::make_unique<SwitchArm>();
            arm->is_else = a->is_else;
            arm->line = a->line;
            arm->col = a->col;
            // the alternative of a sum-type arm and its payload bindings
            arm->alt = a->alt;
            arm->binds = a->binds;
            for (const auto& pat : a->pats) {
                SwitchPat q;
                q.lo = pat.lo ? clone_expr(*pat.lo) : nullptr;
                q.hi = pat.hi ? clone_expr(*pat.hi) : nullptr;
                q.is_range = pat.is_range;
                q.inclusive = pat.inclusive;
                arm->pats.push_back(std::move(q));
            }
            arm->body = clone_block(*a->body);
            r->arms.push_back(std::move(arm));
        }
        return r;
    }
    if (dynamic_cast<const BreakStmt*>(&s)) return mk<BreakStmt>(s);
    if (dynamic_cast<const ContinueStmt*>(&s)) return mk<ContinueStmt>(s);
    if (auto p = dynamic_cast<const ReturnStmt*>(&s)) {
        auto r = mk<ReturnStmt>(s);
        r->value = p->value ? clone_expr(*p->value) : nullptr;
        return r;
    }
    if (auto p = dynamic_cast<const ThrowStmt*>(&s)) {
        auto r = mk<ThrowStmt>(s);
        r->value = p->value ? clone_expr(*p->value) : nullptr;
        return r;
    }
    if (auto p = dynamic_cast<const TryStmt*>(&s)) {
        auto r = mk<TryStmt>(s);
        r->body = p->body ? clone_block(*p->body) : nullptr;
        r->catches.reserve(p->catches.size());
        for (const auto& c : p->catches) {
            auto cc = std::make_unique<CatchClause>();
            cc->name = c->name;
            cc->spec = c->spec ? clone_type_spec(*c->spec) : nullptr;
            cc->body = c->body ? clone_block(*c->body) : nullptr;
            cc->line = c->line;
            cc->col = c->col;
            r->catches.push_back(std::move(cc));
        }
        r->finally_body = p->finally_body ? clone_block(*p->finally_body) : nullptr;
        return r;
    }
    unsupported_kind(node_kind(s), "statement");
}

std::unique_ptr<Param> clone_param(const Param& p) {
    auto r = mk<Param>(p);
    r->name = p.name;
    r->spec = p.spec ? clone_type_spec(*p.spec) : nullptr;
    r->is_const = p.is_const;
    r->by_ref = p.by_ref;
    return r;
}

std::unique_ptr<MethodDecl> clone_method(const MethodDecl& m) {
    auto r = mk<MethodDecl>(m);
    r->name = m.name;
    r->ret = m.ret ? clone_type_spec(*m.ret) : nullptr;
    r->ret_ref = m.ret_ref;
    r->is_operator = m.is_operator;
    r->params = clone_params(m.params);
    r->body = m.body ? clone_block(*m.body) : nullptr;
    r->is_override = m.is_override;
    r->is_public = m.is_public;
    return r;
}

std::unique_ptr<CtorDecl> clone_ctor(const CtorDecl& c) {
    auto r = mk<CtorDecl>(c);
    r->params = clone_params(c.params);
    r->body = c.body ? clone_block(*c.body) : nullptr;
    r->is_public = c.is_public;
    return r;
}

std::unique_ptr<DtorDecl> clone_dtor(const DtorDecl& d) {
    auto r = mk<DtorDecl>(d);
    r->body = d.body ? clone_block(*d.body) : nullptr;
    return r;
}

std::unique_ptr<ConstMember> clone_const_member(const ConstMember& c) {
    auto r = mk<ConstMember>(c);
    r->name = c.name;
    r->spec = c.spec ? clone_type_spec(*c.spec) : nullptr;
    r->init = c.init ? clone_expr(*c.init) : nullptr;
    r->is_public = c.is_public;
    return r;
}

std::unique_ptr<Decl> clone_decl(const Decl& d) {
    if (auto p = dynamic_cast<const StructDecl*>(&d)) {
        auto r = mk<StructDecl>(d);
        r->name = p->name;
        r->bases = p->bases;
        r->base_specs = clone_type_args(p->base_specs);
        r->fields = clone_fields(p->fields);
        r->field_visibility = p->field_visibility;
        r->const_members = clone_const_members(p->const_members);
        r->methods = clone_methods(p->methods);
        r->ctors = clone_ctors(p->ctors);
        r->dtor = p->dtor ? clone_dtor(*p->dtor) : nullptr;
        r->is_packed = p->is_packed;
        r->is_union = p->is_union;
        r->align_expr = p->align_expr ? clone_expr(*p->align_expr) : nullptr;
        r->type_params = clone_type_params(p->type_params);
        r->is_public = p->is_public;
        r->is_template = p->is_template;
        return r;
    }
    if (auto p = dynamic_cast<const SumDecl*>(&d)) {
        auto r = mk<SumDecl>(d);
        r->name = p->name;
        for (const auto& alt : p->alts) {
            SumAlt copy;
            copy.name = alt.name;
            copy.line = alt.line;
            copy.col = alt.col;
            for (const auto& fs : alt.fields) copy.fields.push_back(fs ? clone_type_spec(*fs) : nullptr);
            r->alts.push_back(std::move(copy));
        }
        r->type_params = clone_type_params(p->type_params);
        r->is_public = p->is_public;
        r->is_template = p->is_template;
        return r;
    }
    if (auto p = dynamic_cast<const ClassDecl*>(&d)) {        auto r = mk<ClassDecl>(d);
        r->name = p->name;
        r->bases = p->bases;
        r->base_specs = clone_type_args(p->base_specs);
        r->fields = clone_fields(p->fields);
        r->field_visibility = p->field_visibility;
        r->const_members = clone_const_members(p->const_members);
        r->methods = clone_methods(p->methods);
        r->ctors = clone_ctors(p->ctors);
        r->dtor = p->dtor ? clone_dtor(*p->dtor) : nullptr;
        r->type_params = clone_type_params(p->type_params);
        r->is_public = p->is_public;
        r->is_template = p->is_template;
        return r;
    }
    if (auto p = dynamic_cast<const InterfaceDecl*>(&d)) {
        auto r = mk<InterfaceDecl>(d);
        r->name = p->name;
        r->bases = p->bases;
        r->base_specs = clone_type_args(p->base_specs);
        r->methods = clone_methods(p->methods);
        r->type_params = clone_type_params(p->type_params);
        r->is_public = p->is_public;
        r->is_template = p->is_template;
        return r;
    }
    if (auto p = dynamic_cast<const FuncDecl*>(&d)) {
        auto r = mk<FuncDecl>(d);
        r->name = p->name;
        r->ret = p->ret ? clone_type_spec(*p->ret) : nullptr;
        r->ret_ref = p->ret_ref;
        r->params = clone_params(p->params);
        r->body = p->body ? clone_block(*p->body) : nullptr;
        r->is_extern = p->is_extern;
        r->is_varargs = p->is_varargs;
        r->is_operator = p->is_operator;
        r->type_params = clone_type_params(p->type_params);
        r->is_public = p->is_public;
        r->is_template = p->is_template;
        return r;
    }
    if (auto p = dynamic_cast<const EnumDecl*>(&d)) {
        auto r = mk<EnumDecl>(d);
        r->name = p->name;
        r->members = p->members;
        r->is_public = p->is_public;
        return r;
    }
    if (auto p = dynamic_cast<const GlobalVar*>(&d)) {
        auto r = mk<GlobalVar>(d);
        r->name = p->name;
        r->spec = p->spec ? clone_type_spec(*p->spec) : nullptr;
        r->init = p->init ? clone_init(*p->init) : nullptr;
        r->is_extern = p->is_extern;
        r->is_const = p->is_const;
        r->is_public = p->is_public;
        return r;
    }
    if (auto p = dynamic_cast<const TypeAliasDecl*>(&d)) {
        auto r = mk<TypeAliasDecl>(d);
        r->name = p->name;
        r->spec = p->spec ? clone_type_spec(*p->spec) : nullptr;
        r->is_public = p->is_public;
        return r;
    }
    if (auto p = dynamic_cast<const UseDecl*>(&d)) {
        auto r = mk<UseDecl>(d);
        r->unit_name = p->unit_name;
        r->is_public = p->is_public;
        return r;
    }
    unsupported_kind(node_kind(d), "declaration");
}

}  // namespace muc_detail
