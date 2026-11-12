unit throw_non_exception;

class NotErr {
    int x;
}

int main() {
    throw new NotErr();
    return 0;
}
