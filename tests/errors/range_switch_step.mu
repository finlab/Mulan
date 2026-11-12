unit rs2;

int main() {
    int x = 5;
    switch x {
        0..10 step 2: {
            return 1;
        }
        else: {
            return 0;
        }
    }
}
