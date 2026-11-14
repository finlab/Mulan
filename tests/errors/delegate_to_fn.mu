unit delegate_to_fn;
type D = delegate (int) => int;
type F = (int) => int;

int main() {
    D d = null;
    F f = d;
    return 0;
}
