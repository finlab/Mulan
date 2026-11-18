// A `const class` can implement an interface whose member is declared `const`: the implementation's
// methods are implicitly `const`, which satisfies a read-only contract exactly. Through a `const I` view
// only those methods are reachable.
unit const_class_iface;

extern int printf(const char* fmt, ...);

interface Sized { int size() const; }
interface Named { string name() const; }

const class Item : Sized, Named {
public:
    int n;
    string label;

    Item(int k) : n(k), label("item") { }

    override int size() {
        return n;
    }

    override string name() {
        return label;
    }
}

int main() {
    Item it = new Item(3);
    Sized s = it;
    Named nm = it;
    if s.size() != 3 { printf("size %d\n", s.size()); return 1; }
    if nm.name() != "item" { printf("name\n"); return 2; }
    const Sized cs = s;
    if cs.size() != 3 { printf("const size %d\n", cs.size()); return 3; }
    printf("const_class_iface ok\n");
    return 0;
}
