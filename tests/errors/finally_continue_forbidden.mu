unit finally_continue_forbidden;

int main() {
    while true {
        try {
            continue;
        } finally {
            continue;
        }
    }
    return 0;
}
