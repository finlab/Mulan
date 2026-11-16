unit single_thread_para_for;

use std.cstdio;

int main() {
    int sum = 0;
    para for i in 0..4 {
        int t = i * 2;
        printf("%d\n", t);
    }
    return sum;
}
