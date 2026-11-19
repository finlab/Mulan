unit sum_extern_field;
variant Option<T> {
    Some T;
    None;
}

struct Holder { public Option<int> o; }

extern void take_holder(Holder h);

int main() {
    return 0;
}
