unit addr_rules;

// U21：允许的取址形式（纯值 struct / 定长数组元素 / 指针变量）
extern int puts(const char* s);

struct P { int x; int y; }
struct Outer { P p; int[3] v; }

int main() {
    P a = {1, 2};
    int* px = &a.x;
    P* pa = &a;
    int[3] arr = {3, 4, 5};
    int* pe = &arr[1];
    int n = 6;
    int* pn = &n;
    int** pp = &pn;
    Outer o = {{7, 8}, {9, 10, 11}};
    int* po = &o.p.y;
    int* pv = &o.v[2];

    if *px != 1 or (*pa).x != 1 or *pe != 4 or **pp != 6 or *po != 8 or *pv != 11 {
        puts("addr rules failed");
        return 1;
    }
    puts("addr rules ok");
    return 0;
}
