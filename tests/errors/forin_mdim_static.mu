unit forin_mdim_static;

// for-in is not supported for static multi-dimensional arrays
int main() {
    int[2][3] m = {};
    for x in m { }
    return 0;
}
