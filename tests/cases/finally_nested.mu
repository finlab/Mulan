unit finally_nested;

extern int printf(const char* fmt, ...);

int h() {
    try {
        try {
            return 7;
        } finally {
            printf("inner\n");
        }
    } finally {
        printf("outer\n");
    }
}

int main() {
    printf("h=%d\n", h());
    return 0;
}
