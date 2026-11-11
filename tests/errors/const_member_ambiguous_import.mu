unit const_member_ambiguous_import;

use lib_const_ambig_a;
use lib_const_ambig_b;

const int X = K;

int main() {
    return X;
}
