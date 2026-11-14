// charpad.mu — char[N] initialized from a string: copy contents, append NUL, zero the rest
unit charpad;

extern int puts(const char* s);

int main() {
    char[8] b = "ab";
    // byte 2 is the NUL, the remaining bytes must be zeroed
    if b[2] == 0 and b[7] == 0 and sizeof(b) / sizeof(b[0]) == 8 {
        puts("zero-padded ok");
    }
    puts(&b[0]);
    return 0;
}
