unit adv;

extern int puts(readonly char* s);

struct Box { int[3] v; }

int main() {
    int[3] arr = {1, 2, 3};
    int[] s = arr;
    for ref v in s { v *= 3; }
    if arr[0] == 3 and arr[2] == 9 { puts("slice ref ok"); }

    Box box = 0;
    box.v[0] = 7;
    if box.v[0] == 7 { puts("struct array field ok"); }

    int c = 0;
    for i in 0..3 {
        for j in 0..3 {
            if j == 1 { continue; }
            if i == 2 { break; }
            c += 1;
        }
    }
    if c == 4 { puts("nested loop ok"); }

    int big = 300;
    byte small = big as byte;
    if small == 44 { puts("trunc ok"); }

    int* p0 = &arr[0];
    int* p2 = &arr[2];
    isize d = p2 - p0;
    if d == 2 { puts("ptr diff ok"); }

    string s2 = "a\tb";
    if len(s2) == 3 and s2[1] == 9 { puts("escape ok"); }

    int x = 1;
    if true {
        int x = 2;
        if x == 2 { puts("shadow ok"); }
    }
    if x == 1 { puts("outer ok"); }
    return 0;
}


