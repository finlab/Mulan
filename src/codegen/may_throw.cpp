// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Zhang Guolong

// Conservative module-local may-throw analysis (see may_throw.hpp).
//
// owns:      the conservative module-local may-throw analysis that lets call sites skip the pending-slot check
// not:       exception codegen itself (codegen_eh.cpp)
// pass:      between body checking and emission
// concept:   may-throw
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
    return method_cname(sig->owner->unit, sig->owner->name, sig->name,
                  method_symbol_params(sig->params, sig->params_ref), sig->is_const);
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

    // A method (or interface default) called from the body: a struct's concrete method has a visible
    // body; a class/interface dispatch may reach an override the analysis cannot see.
    void addMethodDep(Item& it, const MethodSig* sig) {
        if (!sig) return;
        if (sig->owner && sig->owner->kind == TypeKind::Struct) {
            addDep(it, method_key(sig));
        } else {
            it.unknown = true;
        }
    }

    // The analysis-side approximation of "this expression already owns a fresh value"; a borrowed
    // argument makes the callee run the copy constructor, which can throw.
    static bool ownedExpr(Expr* e) {
        if (!e) return false;
        if (e->conv) return true;
        if (dynamic_cast<NewExpr*>(e) || dynamic_cast<Lambda*>(e) || dynamic_cast<Ternary*>(e) ||
            dynamic_cast<MatchExpr*>(e) || dynamic_cast<MoveExpr*>(e)) {
            return true;
        }
        if (auto c = dynamic_cast<Call*>(e)) return !c->by_ref;
        if (auto b = dynamic_cast<Binary*>(e)) return (b->op_func || b->op_method) && !b->by_ref;
        if (auto u = dynamic_cast<Unary*>(e)) return u->op_func || u->op_method;
        if (auto ix = dynamic_cast<Index*>(e)) return ix->get_method && !ix->by_ref;
        if (auto c = dynamic_cast<Cast*>(e)) return ownedExpr(c->expr.get());
        return false;
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
        // A by-value struct argument runs the user copy constructor when the source is borrowed; that
        // constructor may throw even though no `Call` node names it (#201).
        for (size_t i = 0; i < c->args.size(); ++i) {
            Expr* a = c->args[i].get();
            if (!a) continue;
            TypePtr pt = nullptr;
            if (c->func && i < c->func->pty.size()) pt = c->func->pty[i];
            else if (c->method && i < c->method->params.size()) pt = c->method->params[i];
            else if (c->ctor_pty && i < c->ctor_pty->size()) pt = (*c->ctor_pty)[i];
            if (!pt) continue;
            if (pt->kind == TypeKind::Struct && pt->has_copy_ctor && !ownedExpr(a)) {
                addDep(it, copy_ctor_cname(pt->unit, pt->name));
            }
            // Copying a variant payload / delegate environment may run a user copy constructor.
            if ((pt->kind == TypeKind::Sum || pt->kind == TypeKind::Delegate) && pt->needs_cleanup()) {
                it.direct = true;
            }
        }
    }

    void visitExpr(Expr* e, Item& it) {
        if (!e) return;
        // An implicit constructor (`implicit string(const char*)`) runs at the conversion site and may throw.
        if (e->conv) addDep(it, e->conv->cname);
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
                    // The allocation site runs this class's member step itself (evalNew), because the
                    // inherited default constructor only covers the base subobject.
                    addMemberStepDeps(it, n->class_ty);
                }
            } else if (n->is_array && n->elem_ty) {
                // `new S[n]` runs the element default constructor n times; it can throw (#238).
                addValueConstructionDep(it, n->elem_ty);
            }
            for (auto& d : n->dims) visitExpr(d.get(), it);
            for (auto& a : n->args) visitExpr(a.get(), it);
            for (auto& [name, a] : n->named) visitExpr(a.get(), it);
            return;
        }
        if (auto b = dynamic_cast<Binary*>(e)) {
            if (b->op_func) addDep(it, b->op_func->cname);
            // A member operator is a method call just like a free one; missing it silently dropped
            // the exception its body throws.
            if (b->op_method) addMethodDep(it, b->op_method);
            // `/`, `%`, shifts and numeric-edge arithmetic fail on a runtime cold path.
            if (b->op == "/" || b->op == "%" || b->op == "<<" || b->op == ">>") it.direct = true;
            visitExpr(b->left.get(), it);
            visitExpr(b->right.get(), it);
            return;
        }
        if (auto u = dynamic_cast<Unary*>(e)) {
            if (u->op_func) addDep(it, u->op_func->cname);
            if (u->op_method) addMethodDep(it, u->op_method);
            if (u->op == "*") it.direct = true;  // a null pointer dereference throws
            visitExpr(u->operand.get(), it);
            return;
        }
        if (auto n = dynamic_cast<NotExpr*>(e)) {
            visitExpr(n->operand.get(), it);
            return;
        }
        if (auto ix = dynamic_cast<Index*>(e)) {
            // `c[i]` through a user operator is a method call (the built-in subscript has no method).
            if (ix->get_method) addMethodDep(it, ix->get_method);
            // The built-in subscript is bounds-checked (and a null slice/pointer is checked).
            it.direct = true;
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
            // A numeric `as` has a range-check cold path (NumericEdgeError).
            it.direct = true;
            visitExpr(c->expr.get(), it);
            return;
        }
        if (auto m = dynamic_cast<Member*>(e)) {
            // A field access through a class handle or pointer carries a null check.
            if (m->obj && m->obj->ty) {
                const TypeKind k = m->obj->ty->kind;
                if (k == TypeKind::Class || k == TypeKind::Interface || k == TypeKind::Ptr) {
                    it.direct = true;
                }
            }
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
        if (auto ise = dynamic_cast<IsExpr*>(e)) {
            visitExpr(ise->obj.get(), it);
            return;
        }
        if (auto sw = dynamic_cast<SwapExpr*>(e)) {
            // the byte exchange itself runs no user code, but taking the address of a user subscript does
            visitExpr(sw->a.get(), it);
            visitExpr(sw->b.get(), it);
            return;
        }
        if (auto m = dynamic_cast<MatchExpr*>(e)) {
            visitExpr(m->scrut.get(), it);
            for (auto& a : m->arms) {
                if (a->guard) visitExpr(a->guard.get(), it);
                if (a->value) visitExpr(a->value.get(), it);
                if (a->body) visitBlock(a->body.get(), it);
            }
            return;
        }
        if (auto mv = dynamic_cast<MoveExpr*>(e)) {
            // `move x` runs the user move constructor for a struct that declares one; the constructor
            // body can throw but no `Call` node represents it (#201).
            if (mv->operand && mv->operand->ty && mv->operand->ty->kind == TypeKind::Struct &&
                mv->operand->ty->has_move_ctor) {
                addDep(it, move_ctor_cname(mv->operand->ty->unit, mv->operand->ty->name));
            }
            visitExpr(mv->operand.get(), it);
            return;
        }
        // Leaf kinds (a literal, a name, `sizeof(expr)` whose operand is not evaluated) run no user
        // code. Any other kind is one this walker does not know: stay conservative instead of
        // silently classifying the function as nothrow (a missed exception check is a wrong program).
        if (dynamic_cast<Name*>(e) || dynamic_cast<IntLit*>(e) || dynamic_cast<FloatLit*>(e) ||
            dynamic_cast<CharLit*>(e) || dynamic_cast<StringLit*>(e) || dynamic_cast<BoolLit*>(e) ||
            dynamic_cast<NullLit*>(e) || dynamic_cast<SizeofExpr*>(e)) {
            return;
        }
        it.unknown = true;
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
        it.unknown = true;
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
                // `S y = x;` copies through the user copy constructor, which can throw.
                if (auto ie = dynamic_cast<InitExpr*>(lv->init.get())) {
                    if (lv->rty && lv->rty->kind == TypeKind::Struct && lv->rty->has_copy_ctor &&
                        !ownedExpr(ie->expr.get())) {
                        addDep(it, copy_ctor_cname(lv->rty->unit, lv->rty->name));
                    }
                }
            } else {
                // A default-initialized local constructs struct fields / array elements; an element
                // default constructor that throws must make this function may-throw (#238).
                addValueConstructionDep(it, lv->rty);
            }
            return;
        }
        if (auto es = dynamic_cast<ExprStmt*>(s)) {
            visitExpr(es->expr.get(), it);
            return;
        }
        if (auto as = dynamic_cast<AssignStmt*>(s)) {
            if (as->set_func) addDep(it, as->set_func->cname);
            if (as->set_method) addMethodDep(it, as->set_method);
            // A struct assignment goes through copy-and-swap: the synthesized copy constructor is not
            // a Call node, so add its dependency from the target type (#241).
            TypePtr tgt = as->target ? as->target->ty : nullptr;
            if (tgt && tgt->kind == TypeKind::Struct) {
                if (tgt->has_copy_ctor) addDep(it, copy_ctor_cname(tgt->unit, tgt->name));
                if (tgt->has_move_ctor) addDep(it, move_ctor_cname(tgt->unit, tgt->name));
            }
            if (tgt && (tgt->kind == TypeKind::Sum || tgt->kind == TypeKind::Delegate) &&
                tgt->needs_cleanup()) {
                it.direct = true;  // payload/environment copy can run user code
            }
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
        if (auto cs = dynamic_cast<ForStmt*>(s)) {
            for (auto& st : cs->init) visitStmt(st.get(), it);
            visitExpr(cs->cond.get(), it);
            if (cs->step) visitStmt(cs->step.get(), it);
            visitBlock(cs->body.get(), it);
            return;
        }
        if (auto fs = dynamic_cast<ForInStmt*>(s)) {
            visitExpr(fs->seq.get(), it);
            visitExpr(fs->step.get(), it);
            if (fs->idx_method) addMethodDep(it, fs->idx_method);
            // Cursor form: iterator(), next() and current() are user methods whose bodies may throw
            // (#224); they are not Call nodes in the loop body.
            addMethodDep(it, fs->iter_sig);
            addMethodDep(it, fs->next_sig);
            addMethodDep(it, fs->cur_sig);
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
        if (auto m = dynamic_cast<MatchStmt*>(s)) {
            visitExpr(m->scrut.get(), it);
            for (auto& a : m->arms) {
                if (a->guard) visitExpr(a->guard.get(), it);
                if (a->value) visitExpr(a->value.get(), it);
                if (a->body) visitBlock(a->body.get(), it);
            }
            return;
        }
        // BreakStmt / ContinueStmt: no calls.
        if (dynamic_cast<BreakStmt*>(s) || dynamic_cast<ContinueStmt*>(s)) return;
        // An unknown statement kind (a `para` section, say) runs code this walker cannot see:
        // classify the enclosing function as throwing rather than miss an exception.
        it.unknown = true;
    }

    // A value-typed subobject with a constructor is built before the enclosing constructor's body
    // (emitMemberInits / emitCtorGlue), and that call can throw. The member step is synthesized in
    // codegen, so it is invisible to the body walk above; mirroring it here keeps a constructor that
    // only throws from a member (or a member's member) classified as throwing (#24).
    void addValueConstructionDep(Item& it, const TypePtr& ty) {
        if (!ty || !ty->needs_ctor()) return;
        if (ty->kind == TypeKind::Struct) {
            if (ty->has_default_ctor()) {
                it.deps.push_back(ctor_cname(ty->unit, ty->name, {}));
                return;
            }
            // No user default constructor: the member step constructs this level's fields inline.
            for (const auto& f : ty->fields) addValueConstructionDep(it, f.type);
            return;
        }
        if (ty->kind == TypeKind::Array) addValueConstructionDep(it, ty->elem);
    }

    // Deps for the member step of `owner`: its own fields only, because a class's inherited fields
    // belong to the base subobject's constructor chain.
    void addMemberStepDeps(Item& it, const TypePtr& owner) {
        if (!owner) return;
        size_t first = 0;
        if (owner->kind == TypeKind::Class && owner->base && owner->base->kind == TypeKind::Class) {
            first = owner->base->fields.size();
        }
        for (size_t i = first; i < owner->fields.size(); ++i) {
            if (owner->fields[i].weak) continue;
            addValueConstructionDep(it, owner->fields[i].type);
        }
    }

    void analyzeItem(Item& it) {
        if (it.body) visitBlock(it.body, it);
        if (it.ctor) {
            // The constructor prologue evaluates every `: field(expr)` / base / delegate argument;
            // the expressions are not in the body and can throw (#184).
            for (auto& item : it.ctor->init_list) {
                for (auto& a : item.args) visitExpr(a.get(), it);
            }
        }
        if (it.ctor && it.ownerClass) {
            // A delegating constructor calls another constructor of the same type, and a constructor whose
            // type has a class base calls it — both from the prologue, not from the body (SPEC 3.12), so the
            // dependency has to be added here.
            if (it.ctor->is_delegating) {
                addDep(it, ctor_cname(it.ownerClass->unit, it.ownerClass->name, it.ctor->delegate_pty));
            } else {
                addMemberStepDeps(it, it.ownerClass);
                TypePtr base = it.ownerClass->base;
                if (base && base->kind == TypeKind::Class) {
                    std::string key = it.ctor->has_base_call
                                          ? ctor_cname(base->unit, base->name, it.ctor->base_pty)
                                          : default_ctor_key(base);
                    if (!key.empty()) addDep(it, key);
                }
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
