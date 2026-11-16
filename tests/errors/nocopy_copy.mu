unit nocopy_copy; nocopy struct Handle { int fd; } struct Holder { Handle h; int n; } int main(){ Holder a; Holder b = a; return b.n; }
