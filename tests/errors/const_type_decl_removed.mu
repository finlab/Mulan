// `const struct` / `const class` was removed: const-ness is written per field and per method.
unit const_type_decl_removed;

const struct P { public int x; }

int main() { return 0; }
