// unit_init_string.mu — a String unit-level variable (reference-counted, initialized at runtime)
unit unit_init_string;

extern int puts(const char* s);

String greeting = "hello unit";

int main() {
    puts(greeting.cstr());
    greeting = "replaced";
    puts(greeting.cstr());
    return 0;
}
