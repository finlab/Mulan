unit arrow_nonptr;

struct S { int x; }

int main() {
    S s = {};
    return s->x;
}
