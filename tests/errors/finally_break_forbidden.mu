unit finally_break_forbidden;

int main() {
    while true {
        try {
            break;
        } finally {
            break;
        }
    }
    return 0;
}
