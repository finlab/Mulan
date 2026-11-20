// generic_arg_target — an argument that needs a target type (`Some(3)`) is typed by the concrete
// parameter after instantiation; the result target supplies the type parameter.
unit generic_arg_target;

extern int puts(const char* s);

variant Maybe {
    Some int;
    None;
}

T unwrap<T>(Maybe m) {
    return 0;
}

int main() {
    int a = unwrap(Some(3));
    if a != 0 { puts("value"); return 1; }
    puts("generic arg target ok");
    return 0;
}
