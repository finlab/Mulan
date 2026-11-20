unit option_nomove_move;
use std.option;
nomove struct P { int x; }
int main() { P p = {}; Option<P> a = Some(p); Option<P> b = move a; return 0; }
