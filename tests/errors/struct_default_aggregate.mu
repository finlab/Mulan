unit audit_h2_aggregate;
extern int printf(const char* fmt, ...);

struct P {
    public const int x = 7;
    public int y;
}

int main() {
    P p = {y = 2};
    printf("p.x=%d p.y=%d\n", p.x, p.y);
    P q = {};
    printf("q.x=%d q.y=%d\n", q.x, q.y);
    return 0;
}
