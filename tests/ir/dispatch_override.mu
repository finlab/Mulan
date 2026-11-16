// An override must never be devirtualized away: `via_base` is called with a Derived instance, so the
// call has to stay indirect. (`plain` has no override anywhere and is allowed to become a direct call.)
unit dispatch_override;

class Base {
    virtual int m() {
        return 1;
    }
    int plain() {
        return 3;
    }
}

class Derived : Base {
    override int m() {
        return 2;
    }
}

int via_base(Base b) {
    return b.m();
}

int via_derived(Derived d) {
    return d.m();
}

int main() {
    return via_base(new Derived()) + via_derived(new Derived());
}
