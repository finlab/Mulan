unit member_op_iface;
interface IAddable { Self operator+(Self a, Self b); }
struct M {
    int v;
    M operator+(M o) { return M(v + o.v); }
}
int sum<T: IAddable>(T a, T b) { return (a + b).v; }
int main() { M x = {2}; M y = {3}; return sum<M>(x, y); }
