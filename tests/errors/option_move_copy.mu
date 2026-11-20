unit option_move_copy;
use std.option;
nocopy struct V { int[] a; int n; V() {} }
int main() { Option<V> a = Some(V()); Option<V> b = a; return 0; }
