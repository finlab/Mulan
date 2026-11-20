// string_c_str_null — a null `const char*` from a C API compares as the empty string and does not
// dereference null (#231).
unit string_c_str_null;

extern int puts(const char* s);
extern const char* getenv(const char* name);

int main() {
    string s = "";
    if s == getenv("MULAN_QC_NOT_SET_97531") {
        puts("string c_str null ok");
        return 0;
    }
    puts("not equal");
    return 1;
}
