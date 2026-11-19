unit alt_shadow_both;
use std.cstdio;

// A user function with the same name as a sum alternative keeps serving its own call sites, while a
// sum-typed target still constructs the alternative (`Option<int> o = Some(3);`).
variant Option<T> {
    Some T;
    None;
}

int Some(int x) {
    return x * 2;
}

int main() {
    int doubled = Some(21);
    if doubled != 42 {
        printf("function lost: %d\n", doubled);
        return 1;
    }
    Option<int> o = Some(3);
    if o is Some(v) {
        if v != 3 {
            printf("bad payload %d\n", v);
            return 2;
        }
    } else {
        printf("alternative lost\n");
        return 3;
    }
    printf("shadow ok\n");
    return 0;
}
