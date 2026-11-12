unit finally_return_forbidden;

int main() {
    try {
        return 0;
    } finally {
        return 1;
    }
}
