unit structs3;
extern int puts(const char* s);

struct Pt { int x, y; }
struct Box { int[3] v; }
struct Rec { Pt p; char[8] name; int[3] nums; }

int main() {
    Box b = {{1, 2, 3}};
    if b.v[0] == 1 and b.v[2] == 3 { puts("box init ok"); }

    Pt[2] ps = {{4, 5}, {6, 7}};
    if ps[1].x == 6 and ps[0].y == 5 { puts("array of struct ok"); }

    Rec r = {{8, 9}, "hi", {1, 2, 3}};
    if r.p.x == 8 and r.name[0] == 104 and r.nums[2] == 3 { puts("nested ok"); }

    Rec q = {p = {10, 11}, name = "yo", nums = {4, 5, 6}};
    if q.p.y == 11 and q.name[1] == 111 and q.nums[0] == 4 { puts("named nested ok"); }
    return 0;
}
