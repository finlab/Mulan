unit generic_arg_no_target;
variant Maybe { Some int; None; }
T unwrap<T>(Maybe m) { return 0; }
int main() { unwrap(Some(3)); return 0; }
