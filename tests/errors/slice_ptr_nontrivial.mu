unit slice_ptr_nontrivial;

struct S { public: string s; }

int main() {
    // a raw write through the pointer would bypass the element's retain/release
    S[] a = new S[2];
    S* p = a.ptr();
    return 0;
}
