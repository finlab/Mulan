unit const_return_fn;
use std.cstdio;

// `const` at unit level used to be recognized only as a variable declaration; the readonly return
// type form (`const char* f()`) failed to parse because the identifier was followed by `(`.
const char* greeting() {
    return "hello";
}

const int answer() {
    return 42;
}

const char* pick(bool left) {
    return left ? "L" : "R";
}

int main() {
    const char* g = greeting();
    int a = answer();
    printf("%s %d %s\n", g, a, pick(false));
    if a != 42 {
        return 1;
    }
    return 0;
}
