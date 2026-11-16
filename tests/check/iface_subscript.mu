unit iface_subscript;

// A constraint may declare a subscript: the receiver is spelled `Self`, the operand after it is the
// index and is deliberately *not* `Self`.
interface IIndex<T> {
    isize length();
    T operator[](Self c, isize i);
}

// member spelling: the receiver is the implicit `this`
struct Member {
    int[] d;

    isize length() { return d.length(); }
    int operator[](isize i) { return d[i]; }
}

// a borrow satisfies the declared value return
struct Borrowed {
    int[] d;

    isize length() { return d.length(); }
    ref int operator[](isize i) { return d[i]; }
}

int total<T: IIndex<int>>(T c) {
    int s = 0;
    for i in 0..c.length() {
        s += c[i];
    }
    return s;
}

int main() {
    Member m = {};
    m.d = new int[2];
    m.d[0] = 1;
    m.d[1] = 2;

    Borrowed b = {};
    b.d = new int[2];
    b.d[0] = 1;
    b.d[1] = 2;

    int a = total<Member>(m);
    int e = total<Borrowed>(b);
    return a * 100 + e;
}
