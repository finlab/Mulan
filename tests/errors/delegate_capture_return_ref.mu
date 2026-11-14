unit delegate_capture_return_ref;
type Get = delegate () => int;

Get bad(int v) {
    return [ref v] () => v;
}

int main() {
    return 0;
}
