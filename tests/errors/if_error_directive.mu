unit if_error_directive;

#if ptr_bits > 0 {
#error "this platform is not supported"
}

int main() { return 0; }
