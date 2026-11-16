// A class may declare member operators (build 160) — they are static-dispatch, with the handle as a
// borrowed `this`. `==` / `!=` stay identity comparison and are rejected as members.
unit member_op_class;

class C {
    int v;
    bool operator==(C o) {
        return true;
    }
}

int main() {
    return 0;
}
