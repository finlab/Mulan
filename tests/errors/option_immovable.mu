unit option_immovable;
use std.option;
nocopy nomove struct Q { int x; }
int main() { Option<Q> o = None; return 0; }
