unit multidim_forin_len_hint;
// The hint must name the API that exists: `a.length(k)`. The old hint recommended `len(a, k)`,
// which is a removed builtin, so following it hit a second error.
int main() {
    int[,] m = new int[2,3];
    int n = 0;
    for x in m {
        n += 1;
    }
    return 0;
}
