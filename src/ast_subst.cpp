// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// AST type parameter substitution implementation.
#include "ast_subst.hpp"

#include <utility>

#include "ast_clone.hpp"

namespace muc_detail {

void subst_type_spec(TypeSpec& s, const TypeBindings& b) {
    if (s.kind == TypeSpec::K::Named && s.args.empty()) {
        auto it = b.find(s.name);
        if (it != b.end()) {
            // Preserves the const / weak modifiers at the original position (`const T` → `const <type argument>`)
            const bool keep_const = s.is_const;
            const bool keep_weak = s.weak;
            TypeSpecPtr rep = clone_type_spec(*it->second);
            s = std::move(*rep);
            s.is_const = s.is_const || keep_const;
            s.weak = s.weak || keep_weak;
            return;  // the type argument is already complete, no need to recurse further
        }
    }
    for (auto& a : s.args) {
        if (a) subst_type_spec(*a, b);
    }
    if (s.elem) subst_type_spec(*s.elem, b);
}

void subst_expr(Expr& e, const TypeBindings& b) {
    if (auto p = dynamic_cast<Member*>(&e)) {
        subst_expr(*p->obj, b);
        return;
    }
    if (auto p = dynamic_cast<Index*>(&e)) {
        subst_expr(*p->base, b);
        for (auto& i : p->indices) subst_expr(*i, b);
        return;
    }
    if (auto p = dynamic_cast<Unary*>(&e)) {
        subst_expr(*p->operand, b);
        return;
    }
    if (auto p = dynamic_cast<NotExpr*>(&e)) {
        subst_expr(*p->operand, b);
        return;
    }
    if (auto p = dynamic_cast<Binary*>(&e)) {
        subst_expr(*p->left, b);
        subst_expr(*p->right, b);
        return;
    }
    if (auto p = dynamic_cast<Cast*>(&e)) {
        subst_expr(*p->expr, b);
        if (p->spec) subst_type_spec(*p->spec, b);
        return;
    }
    if (auto p = dynamic_cast<Ternary*>(&e)) {
        subst_expr(*p->cond, b);
        subst_expr(*p->then_e, b);
        subst_expr(*p->else_e, b);
        return;
    }
    if (auto p = dynamic_cast<Call*>(&e)) {
        subst_expr(*p->callee, b);
        for (auto& a : p->args) subst_expr(*a, b);
        for (auto& n : p->named) subst_expr(*n.second, b);
        return;
    }
    if (auto p = dynamic_cast<NewExpr*>(&e)) {
        if (p->type) subst_type_spec(*p->type, b);
        for (auto& a : p->args) subst_expr(*a, b);
        for (auto& n : p->named) subst_expr(*n.second, b);
        for (auto& d : p->dims) subst_expr(*d, b);
        return;
    }
    if (auto p = dynamic_cast<SizeofExpr*>(&e)) {
        if (p->spec) subst_type_spec(*p->spec, b);
        return;
    }
    if (auto p = dynamic_cast<Len*>(&e)) {
        subst_expr(*p->seq, b);
        if (p->dim) subst_expr(*p->dim, b);
        return;
    }
    if (auto p = dynamic_cast<RangeExpr*>(&e)) {
        subst_expr(*p->lo, b);
        subst_expr(*p->hi, b);
        return;
    }
}

void subst_init(Init& i, const TypeBindings& b) {
    if (auto p = dynamic_cast<InitExpr*>(&i)) {
        subst_expr(*p->expr, b);
        return;
    }
    if (auto p = dynamic_cast<InitAggregate*>(&i)) {
        for (auto& item : p->items) {
            if (item.second) subst_init(*item.second, b);
        }
        return;
    }
    if (auto p = dynamic_cast<InitRange*>(&i)) {
        subst_expr(*p->lo, b);
        subst_expr(*p->hi, b);
        return;
    }
}

void subst_block(Block& blk, const TypeBindings& b) {
    for (auto& s : blk.stmts) subst_stmt(*s, b);
}

void subst_stmt(Stmt& s, const TypeBindings& b) {
    if (auto p = dynamic_cast<Block*>(&s)) {
        subst_block(*p, b);
        return;
    }
    if (auto p = dynamic_cast<LocalVar*>(&s)) {
        if (p->spec) subst_type_spec(*p->spec, b);
        if (p->init) subst_init(*p->init, b);
        return;
    }
    if (auto p = dynamic_cast<ExprStmt*>(&s)) {
        subst_expr(*p->expr, b);
        return;
    }
    if (auto p = dynamic_cast<AssignStmt*>(&s)) {
        subst_expr(*p->target, b);
        subst_expr(*p->value, b);
        return;
    }
    if (auto p = dynamic_cast<IfStmt*>(&s)) {
        subst_expr(*p->cond, b);
        subst_block(*p->then, b);
        if (p->els) subst_stmt(*p->els, b);
        return;
    }
    if (auto p = dynamic_cast<WhileStmt*>(&s)) {
        subst_expr(*p->cond, b);
        subst_block(*p->body, b);
        return;
    }
    if (auto p = dynamic_cast<ForInStmt*>(&s)) {
        subst_expr(*p->seq, b);
        if (p->step) subst_expr(*p->step, b);
        subst_block(*p->body, b);
        return;
    }
    if (auto p = dynamic_cast<SwitchStmt*>(&s)) {
        subst_expr(*p->scrut, b);
        for (auto& a : p->arms) {
            for (auto& pat : a->pats) {
                if (pat.lo) subst_expr(*pat.lo, b);
                if (pat.hi) subst_expr(*pat.hi, b);
            }
            subst_block(*a->body, b);
        }
        return;
    }
    if (auto p = dynamic_cast<ReturnStmt*>(&s)) {
        if (p->value) subst_expr(*p->value, b);
        return;
    }
}

namespace {

void subst_params(std::vector<std::unique_ptr<Param>>& params, const TypeBindings& b) {
    for (auto& p : params) {
        if (p->spec) subst_type_spec(*p->spec, b);
    }
}

void subst_methods(std::vector<std::unique_ptr<MethodDecl>>& methods, const TypeBindings& b) {
    for (auto& m : methods) {
        if (m->ret) subst_type_spec(*m->ret, b);
        subst_params(m->params, b);
        if (m->body) subst_block(*m->body, b);
    }
}

void subst_ctors(std::vector<std::unique_ptr<CtorDecl>>& ctors, const TypeBindings& b) {
    for (auto& c : ctors) {
        subst_params(c->params, b);
        if (c->body) subst_block(*c->body, b);
    }
}

void subst_const_members(std::vector<std::unique_ptr<ConstMember>>& list, const TypeBindings& b) {
    for (auto& c : list) {
        if (c->spec) subst_type_spec(*c->spec, b);
        if (c->init) subst_expr(*c->init, b);
    }
}

}  // namespace

void subst_decl(Decl& d, const TypeBindings& b) {
    if (auto p = dynamic_cast<StructDecl*>(&d)) {
        for (auto& f : p->fields) {
            if (f.second) subst_type_spec(*f.second, b);
        }
        subst_const_members(p->const_members, b);
        subst_methods(p->methods, b);
        subst_ctors(p->ctors, b);
        if (p->dtor && p->dtor->body) subst_block(*p->dtor->body, b);
        if (p->align_expr) subst_expr(*p->align_expr, b);
        return;
    }
    if (auto p = dynamic_cast<ClassDecl*>(&d)) {
        for (auto& bs : p->base_specs) {
            if (bs) subst_type_spec(*bs, b);
        }
        for (auto& f : p->fields) {
            if (f.second) subst_type_spec(*f.second, b);
        }
        subst_const_members(p->const_members, b);
        subst_methods(p->methods, b);
        subst_ctors(p->ctors, b);
        if (p->dtor && p->dtor->body) subst_block(*p->dtor->body, b);
        return;
    }
    if (auto p = dynamic_cast<InterfaceDecl*>(&d)) {
        subst_methods(p->methods, b);
        return;
    }
    if (auto p = dynamic_cast<FuncDecl*>(&d)) {
        if (p->ret) subst_type_spec(*p->ret, b);
        subst_params(p->params, b);
        if (p->body) subst_block(*p->body, b);
        return;
    }
    if (auto p = dynamic_cast<GlobalVar*>(&d)) {
        if (p->spec) subst_type_spec(*p->spec, b);
        if (p->init) subst_init(*p->init, b);
        return;
    }
    if (auto p = dynamic_cast<TypeAliasDecl*>(&d)) {
        if (p->spec) subst_type_spec(*p->spec, b);
        return;
    }
}

}  // namespace muc_detail
