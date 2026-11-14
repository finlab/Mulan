// A `ref` parameter is a language-level borrow; a C consumer has pointers, so `export` refuses it
// (write `int*` explicitly instead).
unit export_ref_param;

export void bump(ref int x) {
    x = x + 1;
}

int main() {
    return 0;
}
