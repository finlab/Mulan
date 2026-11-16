// Closed-world devirtualization must never lose an override: a call through a base-class handle runs
// the derived implementation when one exists, and only becomes a direct call when no subclass in the
// program declares that signature (build 159).
unit devirt_dispatch;

extern int printf(const char* fmt, ...);

class Base {
    int value() {
        return 1;
    }
    // overridden by Derived below: every call on a Base handle must dispatch
    virtual int overridden() {
        return 10;
    }
    // nobody overrides this one: the call may be emitted as a direct call
    int not_overridden() {
        return 100;
    }
}

class Derived : Base {
    override int overridden() {
        return 20;
    }
}

// `b` may hold a Base or a Derived: `overridden()` has to go through the vtable
int call_overridden(Base b) {
    return b.overridden();
}

// `d` can only hold a Derived (no subclass of Derived exists): the target is unique
int call_on_derived(Derived d) {
    return d.overridden();
}

// no override anywhere: direct call, still the right result
int call_plain(Base b) {
    return b.not_overridden() + b.value();
}

int main() {
    Base plain = new Base();
    Base derived_as_base = new Derived();
    Derived derived = new Derived();

    // the override is honoured through the base handle
    printf("base %d\n", plain.overridden());
    printf("derived-as-base %d\n", derived_as_base.overridden());
    printf("derived %d\n", call_on_derived(derived));

    // ... and through a function that takes the base class
    printf("call base %d\n", call_overridden(plain));
    printf("call derived %d\n", call_overridden(derived_as_base));

    printf("plain %d\n", call_plain(derived_as_base));
    return 0;
}
