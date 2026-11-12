unit finally_throw_forbidden;

class E : Exception {
    E() { super("e"); }
}

int main() {
    try {
        return 0;
    } finally {
        throw new E();
    }
}
