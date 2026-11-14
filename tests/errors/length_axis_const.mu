unit length_axis_const;

int main() {
    int[,] a = new int[2,3];
    a[0,0] = 42;
    return a.length(2) as int;
}
