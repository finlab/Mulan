unit class_stress2;
extern int puts(const char* s);
struct P { int x, y; }
class Mixed {
    long big;
    double d;
    bool flag;
    string name;
    P p;
    int[3] nums;
    Mixed() {
        big = 1234567890123;
        d = 2.5;
        flag = true;
        name = "mu";
        p = P(3, 4);
        nums[0] = 7; nums[1] = 8; nums[2] = 9;
    }
    ~Mixed() { }
    long total() { return big + nums[0] + nums[1] + nums[2]; }
}
int main() {
    Mixed m = new Mixed();
    if m.big != 1234567890123 { puts("long fail"); return 1; }
    if m.d != 2.5 { puts("double fail"); return 1; }
    if not m.flag { puts("bool fail"); return 1; }
    if m.name != "mu" { puts("string fail"); return 1; }
    if m.p.x != 3 or m.p.y != 4 { puts("struct fail"); return 1; }
    if m.nums[2] != 9 { puts("array fail"); return 1; }
    if m.total() != 1234567890123 + 24 { puts("method fail"); return 1; }
    m = null;
    puts("stress2 ok");
    return 0;
}


