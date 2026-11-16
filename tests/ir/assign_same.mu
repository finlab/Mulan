unit assign_same_ir;
// `b = a` with the identical pointer skips the retain/release pair: the assignment lowering must
// compare the old and the new handle and only enter the retain/release path when they differ.
class Obj {
    public int v;
    Obj(int x) { v = x; }
}

int main() {
    Obj a = new Obj(1);
    Obj b = new Obj(2);
    b = a;
    return b.v;
}
