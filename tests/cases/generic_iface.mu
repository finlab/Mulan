// generic_iface.mu — generic interface as a constraint (M5a): structural satisfaction, no `:` declaration needed
unit generic_iface;

extern int puts(const char* s);

interface ISeq<T> {
    usize length();
    T at(usize i);
}

struct Vec {
    int[] data;

    usize length() { return data.length(); }

    int at(usize i) { return data[i]; }
}

int sum_all<T: ISeq<int>>(T seq) {
    int total = 0;
    for i in 0..seq.length() {
        total = total + seq.at(i);
    }
    return total;
}

int main() {
    Vec v = {new int[3]};
    v.data[0] = 1;
    v.data[1] = 2;
    v.data[2] = 39;

    int s = sum_all<Vec>(v);
    if (s == 42) { puts("generic iface ok"); }
    return 0;
}
