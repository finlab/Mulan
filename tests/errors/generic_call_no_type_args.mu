unit generic_call_no_type_args;

T ident<T>(T v) { return v; }

int main() {
    int a = 3;
    int b = ident(a);
    return b;
}
