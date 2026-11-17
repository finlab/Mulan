// A `const T[N]` array has read-only elements (SPEC 3.7): writing a scalar element is an error.
unit const_arr_scalar_write;

const int[3] ia = {1, 2, 3};

int main() {
    ia[0] = 9;
    return 0;
}
