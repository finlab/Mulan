unit slice_uninit_ref;

int main() {
    // a reference-holding element needs its zero value to be a valid handle
    string[] s = uninit new string[4];
    return 0;
}
