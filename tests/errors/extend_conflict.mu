unit extend_conflict;

use extlib2.first;
use extlib2.second;

struct S : I {
    public int v;
    override int f() { return v; }
}

int main() { return 0; }
