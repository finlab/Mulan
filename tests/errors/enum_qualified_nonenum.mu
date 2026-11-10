unit enum_qualified_nonenum;

struct P { int a; }

int main() {
    int v = P.a;
    return v;
}
