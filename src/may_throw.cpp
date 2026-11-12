// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Conservative module-local may-throw analysis (see may_throw.hpp).
#include "may_throw.hpp"

#include <unordered_map>
#include <utility>
#include <vector>

#include "mangle.hpp"

namespace muc_detail {

namespace {

struct Item {
    std::string cname;
    Block* body = nullptr;
    TypePtr ownerClass;      // class/struct/interface owning a method or ctor
    CtorDecl* ctor = nullptr;
    bool direct = false;     // contains a `throw`
    bool unknown = false;    // calls something whose body is not visible
    std::vector<std::string> deps;
};

std::string method_key(const MethodSig* sig) {
    if (!sig || !sig->owner) return {};
    return method_cname(sig->owner->unit, sig->owner->name, sig->name, sig->params);
}

struct Analysis {
    std::unordered_map<std::string, Item> items;

    void addDep(Item& it, const std::string& key) {
        if (key.empty()) {
            it.unknown = true;
        } else {
            it.deps.push_back(key);
        }
    }

    void visitBlock(Block* b, Item& it) {
        if (!b) return;
        for (auto& s : b->stmts) visitStmt(s.get(), it);
    }

    void visitCall(Call* c, Item& it) {
        if (!c) return;
        if (c->callee_kind == "func") {
            if (c->func) {
                if (!c->func->is_extern) addDep(it, c->func->cname);
            } else {
                it.unknown = true;
            }
        } else if (c->callee_kind == "struct_ctor") {
            if (c->ctor && c->ctor_pty) {
                addDep(it, ctor_cname(c->ctor->unit, c->ctor->name, *c->ctor_pty));
            } else {
                it.unknown = true;
            }
        } else if (c->callee_kind == "struct_method" || c->callee_kind == "method") {
            if (c->method && c->method->owner) {
                if (c->method->owner->kind == TypeKind::Struct) {
                    addDep(it, method_key(c->method));
                } else {
                    // Class/interface dispatch can reach an unknown override.
                    it.unknown = true;
                }
            } else {
                it.unknown = true;
            }
        } else if (c->callee_kind == "super") {
            if (c->method) {
                it.unknown = true;  // class method dispatch through super
            } else if (it.ownerClass && it.ownerClass->base &&
                       it.ownerClass->base->kind == TypeKind::Class) {
                TypePtr base = it.ownerClass->base;
                std::string key = c->ctor_pty ? ctor_cname(base->unit, base->name, *c->ctor_pty)
                                              : default_ctor_key(base);
                if (key.empty()) {
                    it.unknown = true;
                } else {
                    addDep(it, key);
                }
            } else {
                it.unknown = true;
            }
        } else if (c->callee_kind == "builtin" || c->callee_kind == "ctor") {
            // malloc/realloc/free or an aggregate struct construction: no user call.
        } else {
            it.unknown = true;
        }

        if (c->callee) visitExpr(c->callee.get(), it);
        for (auto& a : c->args) visitExpr(a.get(), it);
        for (auto& [name, a] : c->named) visitExpr(a.get(), it);
    }

    void visitExpr(Expr* e, Item& it) {
        if (!e) return;
        if (auto c = dynamic_cast<Call*>(e)) {
            visitCall(c, it);
            return;
        }
        if (auto n = dynamic_cast<NewExpr*>(e)) {
            if (!n->is_array && n->class_ty) {
                if (n->has_ctor) {
                    addDep(it, ctor_cname(n->class_ty->unit, n->class_ty->name, n->ctor_pty));
                } else {
                    std::string key = default_ctor_key(n->class_ty);
                    if (!key.empty()) addDep(it, key);
                }
            }
            for (auto& d : n->dims) visitExpr(d.get(), it);
            for (auto& a : n->args) visitExpr(a.get(), it);
            for (auto& [name, a] : n->named) visitExpr(a.get(), it);
            return;
        }
        if (auto b = dynamic_cast<Binary*>(e)) {
            if (b->op_func) addDep(it, b->op_func->cname);
            visitExpr(b->left.get(), it);
            visitExpr(b->right.get(), it);
            return;
        }
        if (auto u = dynamic_cast<Unary*>(e)) {
            if (u->op_func) addDep(it, u->op_func->cname);
            visitExpr(u->operand.get(), it);
            return;
        }
        if (auto n = dynamic_cast<NotExpr*>(e)) {
            visitExpr(n->operand.get(), it);
            return;
        }
        if (auto ix = dynamic_cast<Index*>(e)) {
            if (ix->get_func) addDep(it, ix->get_func->cname);
            visitExpr(ix->base.get(), it);
            for (auto& idx : ix->indices) visitExpr(idx.get(), it);
            return;
        }
        if (auto t = dynamic_cast<Ternary*>(e)) {
            visitExpr(t->cond.get(), it);
            visitExpr(t->then_e.get(), it);
            visitExpr(t->else_e.get(), it);
            return;
        }
        if (auto c = dynamic_cast<Cast*>(e)) {
            visitExpr(c->expr.get(), it);
            return;
        }
        if (auto m = dynamic_cast<Member*>(e)) {
            visitExpr(m->obj.get(), it);
            return;
        }
        if (auto l = dynamic_cast<Len*>(e)) {
            visitExpr(l->seq.get(), it);
            visitExpr(l->dim.get(), it);
            return;
        }
        if (auto r = dynamic_cast<RangeExpr*>(e)) {
            visitExpr(r->lo.get(), it);
            visitExpr(r->hi.get(), it);
            return;
        }
    }

    void visitInit(Init* init, Item& it) {
        if (!init) return;
        if (auto ie = dynamic_cast<InitExpr*>(init)) {
            visitExpr(ie->expr.get(), it);
            return;
        }
        if (auto ia = dynamic_cast<InitAggregate*>(init)) {
            for (auto& [name, sub] : ia->items) visitInit(sub.get(), it);
            return;
        }
        if (auto ir = dynamic_cast<InitRange*>(init)) {
            visitExpr(ir->lo.get(), it);
            visitExpr(ir->hi.get(), it);
            return;
        }
    }

    void visitStmt(Stmt* s, Item& it) {
        if (!s) return;
        if (auto b = dynamic_cast<Block*>(s)) {
            visitBlock(b, it);
            return;
        }
        if (auto lv = dynamic_cast<LocalVar*>(s)) {
            if (lv->init) {
                visitInit(lv->init.get(), it);
            } else if (lv->rty && lv->rty->kind == TypeKind::Struct && !lv->rty->ctors.empty()) {
                addDep(it, ctor_cname(lv->rty->unit, lv->rty->name, {}));
            }
            return;
        }
        if (auto es = dynamic_cast<ExprStmt*>(s)) {
            visitExpr(es->expr.get(), it);
            return;
        }
        if (auto as = dynamic_cast<AssignStmt*>(s)) {
            if (as->set_func) addDep(it, as->set_func->cname);
            visitExpr(as->target.get(), it);
            visitExpr(as->value.get(), it);
            return;
        }
        if (auto is = dynamic_cast<IfStmt*>(s)) {
            visitExpr(is->cond.get(), it);
            visitBlock(is->then.get(), it);
            visitStmt(is->els.get(), it);
            return;
        }
        if (auto ws = dynamic_cast<WhileStmt*>(s)) {
            visitExpr(ws->cond.get(), it);
            visitBlock(ws->body.get(), it);
            return;
        }
        if (auto fs = dynamic_cast<ForInStmt*>(s)) {
            visitExpr(fs->seq.get(), it);
            visitExpr(fs->step.get(), it);
            if (fs->idx_fn) addDep(it, fs->idx_fn->cname);
            if (fs->len_sig) {
                if (fs->len_sig->owner && fs->len_sig->owner->kind == TypeKind::Struct) {
                    addDep(it, method_key(fs->len_sig));
                } else {
                    it.unknown = true;
                }
            }
            visitBlock(fs->body.get(), it);
            return;
        }
        if (auto sw = dynamic_cast<SwitchStmt*>(s)) {
            visitExpr(sw->scrut.get(), it);
            for (auto& arm : sw->arms) {
                for (auto& pat : arm->pats) {
                    visitExpr(pat.lo.get(), it);
                    visitExpr(pat.hi.get(), it);
                }
                visitBlock(arm->body.get(), it);
            }
            return;
        }
        if (auto ts = dynamic_cast<ThrowStmt*>(s)) {
            it.direct = true;
            visitExpr(ts->value.get(), it);
            return;
        }
        if (auto tr = dynamic_cast<TryStmt*>(s)) {
            visitBlock(tr->body.get(), it);
            for (auto& c : tr->catches) visitBlock(c->body.get(), it);
            // A finally body must not throw; anything escaping it aborts, so it cannot
            // make the enclosing function return with a pending exception.
            return;
        }
        if (auto rs = dynamic_cast<ReturnStmt*>(s)) {
            visitExpr(rs->value.get(), it);
            return;
        }
        // BreakStmt / ContinueStmt: no calls.
    }

    void analyzeItem(Item& it) {
        if (it.body) visitBlock(it.body, it);
        // An omitted `super(...)` becomes an implicit base default-constructor call.
        if (it.ctor && it.ownerClass && !it.ctor->has_explicit_super) {
            TypePtr base = it.ownerClass->base;
            if (base && base->kind == TypeKind::Class) {
                std::string key = default_ctor_key(base);
                if (!key.empty()) addDep(it, key);
            }
        }
    }
};

void addFunctionItem(Analysis& a, const std::string& cname, Block* body, const TypePtr& owner = nullptr,
                     CtorDecl* ctor = nullptr) {
    if (cname.empty() || !body) return;
    Item it;
    it.cname = cname;
    it.body = body;
    it.ownerClass = owner;
    it.ctor = ctor;
    a.items.emplace(cname, std::move(it));
}

}  // namespace

std::string default_ctor_key(const TypePtr& cls) {
    for (TypePtr cur = cls; cur && cur->kind == TypeKind::Class; cur = cur->base) {
        for (auto& params : cur->ctors) {
            if (params.empty()) return ctor_cname(cur->unit, cur->name, {});
        }
    }
    return {};
}

bool MayThrowInfo::mayThrowCname(const std::string& cname) const {
    auto it = funcs.find(cname);
    return it == funcs.end() || it->second;
}

bool MayThrowInfo::mayThrowFunc(const FuncDecl* f) const {
    if (!f || f->is_extern) return false;
    return mayThrowCname(f->cname);
}

bool MayThrowInfo::mayThrowMethod(const MethodSig* sig, const TypePtr& recvTy) const {
    (void)recvTy;
    if (!sig || !sig->owner || sig->owner->kind != TypeKind::Struct) return true;
    return mayThrowCname(method_key(sig));
}

MayThrowInfo computeMayThrow(Program& prog) {
    Analysis a;
    for (auto& u : prog.units) {
        for (auto& d : u->decls) {
            if (d->is_template) continue;
            if (auto f = dynamic_cast<FuncDecl*>(d.get())) {
                if (!f->is_extern) addFunctionItem(a, f->cname, f->body.get());
            } else if (auto sd = dynamic_cast<StructDecl*>(d.get())) {
                for (auto& m : sd->methods) addFunctionItem(a, m->cname, m->body.get(), sd->ty);
                for (auto& c : sd->ctors) addFunctionItem(a, c->cname, c->body.get(), sd->ty, c.get());
                // Struct destructors are nothrow (an escaping exception aborts) and are not queried.
            } else if (auto cd = dynamic_cast<ClassDecl*>(d.get())) {
                for (auto& m : cd->methods) addFunctionItem(a, m->cname, m->body.get(), cd->ty);
                for (auto& c : cd->ctors) addFunctionItem(a, c->cname, c->body.get(), cd->ty, c.get());
            } else if (auto id = dynamic_cast<InterfaceDecl*>(d.get())) {
                for (auto& m : id->methods) addFunctionItem(a, m->cname, m->body.get(), id->ty);
            }
        }
    }

    for (auto& [name, it] : a.items) {
        (void)name;
        a.analyzeItem(it);
    }

    std::unordered_map<std::string, bool> may;
    for (auto& [name, it] : a.items) may[name] = false;
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& [name, it] : a.items) {
            bool v = it.direct || it.unknown;
            if (!v) {
                for (auto& dep : it.deps) {
                    auto found = may.find(dep);
                    if (found == may.end() || found->second) {
                        v = true;
                        break;
                    }
                }
            }
            if (v != may[name]) {
                may[name] = v;
                changed = true;
            }
        }
    }

    MayThrowInfo info;
    info.funcs = std::move(may);
    return info;
}

}  // namespace muc_detail
