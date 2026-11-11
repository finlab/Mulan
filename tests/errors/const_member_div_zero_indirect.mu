unit const_member_div_zero_indirect;

const int A = B;
const int B = 1 / 0;

int main() {
    return A;
}
