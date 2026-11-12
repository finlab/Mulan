unit catch_non_exception;

int main() {
    try {
        return 0;
    } catch (e: int) {
        return 1;
    }
}
