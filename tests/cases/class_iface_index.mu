unit class_iface_index;
use std.cstdio;

// A class implements a subscript contract whose receiver is spelled `Self`: the member form
// (`operator[](isize)`) has the receiver implicit, so `same_sig` must drop it on the contract side.
interface ISeq {
    isize length();
    int operator[](Self c, isize i);
}

class Store : ISeq {
    public int[4] d;
    Store() {
        d[0] = 10;
        d[1] = 11;
        d[2] = 12;
        d[3] = 13;
    }
    override isize length() { return 4; }
    override int operator[](isize i) { return d[i]; }
}

int total<T: ISeq>(T s) {
    int sum = 0;
    for i in 0..s.length() {
        sum += s[i];
    }
    return sum;
}

int main() {
    Store st = new Store();
    int sum = total<Store>(st);
    if sum != 46 {
        printf("bad total %d\n", sum);
        return 1;
    }
    printf("class index ok\n");
    return 0;
}
