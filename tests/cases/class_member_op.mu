// A class may declare member operators (build 160): the receiver is the handle, passed as a borrowed
// `this`, so `c[i]` costs no object copy, `for x in c` needs no free `operator[]`, and the operator may be
// `virtual` when a subclass has to replace it.
unit class_member_op;

extern int printf(const char* fmt, ...);

class Box {
    isize[] data;

    Box(isize n) {
        data = new isize[n];
        isize i = 0;
        while i < n {
            data[i] = i * 10;
            i += 1;
        }
    }

    isize length() {
        return data.length();
    }

    virtual isize operator[](isize i) {
        return data[i];
    }

    isize operator+(isize k) {
        return data[0] + k;
    }
}

class Doubled : Box {
    Doubled(isize n) : Box(n) {
    }

    // a virtual member operator: calls through a `Box` handle must reach this one
    override isize operator[](isize i) {
        return 2 * data[i];
    }
}

int main() {
    Box b = new Box(4);
    isize sum = 0;
    for x in b {
        sum += x;
    }
    printf("sum %d\n", sum as int);
    printf("b2 %d\n", b[2] as int);
    printf("plus %d\n", (b + 5) as int);

    Box as_box = new Doubled(3);
    printf("virtual op %d\n", as_box[1] as int);
    return 0;
}
