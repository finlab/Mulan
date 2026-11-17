// A `const T[N]` array has read-only elements (SPEC 3.7): writing a member through an element is an error
// even though the binding itself is not `const`.
unit const_arr_member_write;

struct P { public int x; }

const P[2] arr = { P(1), P(2) };

int main() {
    arr[0].x = 99;
    return 0;
}
