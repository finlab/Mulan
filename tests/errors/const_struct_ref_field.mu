unit audit_m3_ref_const_struct;
void bump(ref int x) { x += 1; }
const struct P { public int x; public int y; }
int main() { const P p = P(1, 2); bump(p.x); return 0; }
