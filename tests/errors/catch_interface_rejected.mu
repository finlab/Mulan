unit catch_interface;

interface IErr {
    int code();
}

int main() {
    try {
        return 0;
    } catch (e: IErr) {
        return 1;
    }
}
