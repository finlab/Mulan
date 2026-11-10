unit ternary_struct;
struct P { int x; }
struct Q { int y; }
int main() {
    P p = {1};
    Q q = {2};
    bool b = true;
    P r = b ? p : q;
    return r.x;
}
