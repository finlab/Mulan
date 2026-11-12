unit finally_normal;

extern int printf(const char* fmt, ...);

int main() {
    try {
        printf("body\n");
    } finally {
        printf("fin\n");
    }
    printf("after\n");
    return 0;
}
