unit vis_import_ok;
use lib_imp_ok;

int main() {
    Pair p = {1, 2};
    Pair q = Pair(1, 2);
    int a = add2(3);
    int b = K2;
    Color c = Color.Red;
    Color c2 = lib_imp_ok.Color.Green;
    int d = same(1);
    int e = same(1.5);
    return a + b + p.a + q.b + (c as int) + (c2 as int) + d + e;
}
