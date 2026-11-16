// compound_assign.mu — `x op= y` / `x++` on user types (rewritten as `x = x op y`) and on numbers
unit compound_assign;

extern int puts(const char* s);

struct V {
    int x;
    V operator+(V o) { return V(x + o.x); }
    V operator-(V o) { return V(x - o.x); }
    V operator*(int k) { return V(x * k); }
    V operator+(int k) { return V(x + k); }
}

class Box {
public:
    int n;
    Box(int v) { n = v; }
}

Box operator+(Box a, Box b) { Box r = new Box(a.n + b.n); return r; }

interface IAdd { Self operator+(Self a, Self b); }

T twice<T: IAdd>(T a, T b) {
    a += b;
    return a;
}

int main() {
    // member operators on a struct: every compound form goes through `operator<op>`
    V v = {10};
    V w = {3};
    v += w;
    if v.x != 13 { return 1; }
    v -= w;
    if v.x != 10 { return 2; }
    v *= 2;
    if v.x != 20 { return 3; }
    v++;
    if v.x != 21 { return 4; }
    v += 4;
    if v.x != 25 { return 5; }

    // free operator on a class (the handle is shared, the result is a new object)
    Box a = new Box(4);
    Box b = new Box(5);
    a += b;
    if a.n != 9 { return 6; }

    // a library value type: `String` has `operator+` but no `operator+=`
    string s = "ab";
    s += "cd";
    if s.length() != 4 { return 7; }

    // a subscript target (the index is side-effect free, so the rewrite is safe)
    V[] arr = new V[2];
    arr[0] = V(1);
    arr[1] = V(2);
    arr[0] += arr[1];
    if arr[0].x != 3 { return 8; }

    // inside a generic body, checked against the contract at the definition
    V g = twice<V>(V(2), V(5));
    if g.x != 7 { return 9; }

    puts("compound assign ok");
    return 0;
}
