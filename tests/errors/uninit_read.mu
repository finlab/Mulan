// A scalar local read before it is assigned is an error (SPEC 4).
unit uninit_read;
int main() {
    int x;
    return x;
}
