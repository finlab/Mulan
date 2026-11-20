unit option_move_match_lvalue;
use std.option;
nocopy struct V { int[] a; int n; V() {} }
int main() { Option<V> a = Some(V()); match a { Some(x): { } None: { } } return 0; }
