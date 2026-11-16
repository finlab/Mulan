unit sizeof_isize;
use std.cstdio;

// SPEC decision 128: `sizeof` / `alignof` / `offsetof` are `isize`, like `length()` and the
// subscripts. The type must be host-independent (Linux used to see a different type through the
// cstd_posix case, which was a type mismatch in the *test*, not in the compiler).
int main() {
    isize n = sizeof(int);
    isize m = sizeof(void*);
    if n != 4 {
        return 1;
    }
    if m != 8 {
        return 2;
    }
    isize total = sizeof(int) + sizeof(long);
    if total != 12 {
        return 3;
    }
    if sizeof(int) != 4 {
        return 4;
    }
    usize as_size_t = sizeof(int) as usize;
    if as_size_t != 4 {
        return 5;
    }
    printf("sizeof isize ok\n");
    return 0;
}
