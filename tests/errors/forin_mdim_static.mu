unit forin_mdim_static;

// 静态多维数组不支持 for-in
int main() {
    int[2][3] m = {};
    for x in m { }
    return 0;
}
