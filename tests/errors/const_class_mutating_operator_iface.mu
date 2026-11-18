unit audit_m2_operator_receiver;
interface IMutate {
    // A mutating receiver: an implementation may write through the borrow.
    int operator[](ref Self s, isize i);
}

class Store : IMutate {
    int[] data;
    Store() { data = new int[2]; }
    override int operator[](isize i) {
        data[i] = data[i] + 1;
        return data[i];
    }
    int get(isize i) const { return data[i]; }
}

const class Frozen : Store {
public:
    Frozen() : Store() { }
}

int main() {
    Frozen f = new Frozen();
    IMutate view = f;      // must be rejected: the interface can write through its receiver
    return view[0];
}
