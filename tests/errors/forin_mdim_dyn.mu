unit forin_mdim_dyn;

// 动态多维数组不支持 for-in
int main() {
    int[,] a = new int[2,3];
    for i, x in a { }
    return 0;
}
