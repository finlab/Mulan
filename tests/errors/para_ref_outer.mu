unit para_ref_outer;

void bump(ref int x) { x = x + 1; }

int main() {
    int n = 0;
    para {
        { bump(n); }
    }
    return n;
}
