unit generic_target_bound;
interface IOrder { bool operator<(Self a, Self b); }
struct Point { int x; }
T make<T: IOrder>() { throw new Exception("never"); }
int main() { Point p = make(); return p.x; }
