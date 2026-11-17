unit if_body_error;
const bool BAD = true;

int main() {
#if BAD {
#error "unsupported platform"
}
    return 0;
}
