unit forin_mdim_dyn;

// for-in is not supported for dynamic multi-dimensional arrays
int main() {
    int[,] a = new int[2,3];
    for i, x in a { }
    return 0;
}
