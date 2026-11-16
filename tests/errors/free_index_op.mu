// `operator[]` must be a member of the type it indexes (build 161): the free-function form copied its
// receiver on every element access, which is 50x slower in a loop than the member form's borrowed `this`.
unit free_index_op;

struct Vec {
    int[4] data;
    isize n;
    isize length() { return n; }
}

int operator[](Vec v, isize i) { return v.data[i]; }

int main() {
    return 0;
}
