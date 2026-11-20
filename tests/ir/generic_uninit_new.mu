// `uninit new T[n]` must keep the uninitialized allocation after instantiation (the clone used to
// drop NewExpr::uninit, turning malloc into calloc).
unit generic_uninit_new;

T[] make<T>(isize n) { return uninit new T[n]; }

int main() {
    int[] a = make<int>(3);
    a[0] = 5;
    int x = a[0];
    return x;
}
