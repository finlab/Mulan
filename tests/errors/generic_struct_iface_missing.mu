unit generic_struct_iface_missing;
interface IShow<T> { T get(); }
struct Bad<T> : IShow<T> { T v; Bad() {} }
int main() { Bad<int> b = {}; return 0; }
