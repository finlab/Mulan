unit const_global_runtime_write;

use std.string;

const string S = "hi";

int main() {
    S = "x";
    return 0;
}
