unit match_const_type;

int main() {
    string s = "abc";
    int x = match s {
        else: 0;
    };
    return 0;
}
