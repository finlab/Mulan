// A runtime value is not a compile-time constant: the array length must fold (SPEC 1694).
unit const_array_runtime;

int main() {
    int k = 4;
    int[k] a = {};
    return 0;
}
