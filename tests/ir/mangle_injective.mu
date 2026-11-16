unit mangle_injective;

use lib_u;
use lib_u_a;

// Both units contribute one function; the concatenation `lib_u_a` + `b` and `lib_u` + `a_b` must
// produce distinct symbols, or exporting both would silently merge them (#35).
int main() {
    return b(1) + a_b(2) == 6 ? 0 : 1;
}
