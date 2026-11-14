// C has no array results: a C header returns a pointer (or fills a struct), so returning a
// fixed-size array from `extern`/`export` is refused instead of emitting a first-class array.
unit extern_array_return;

extern int[4] ret_arr();

int main() {
    int[4] a = ret_arr();
    return a[0];
}
