unit param_const_read;

class Cat {
public:
    int age;
    Cat() { age = 2; }
    int get() { return age; }
}

struct Point {
    int x;
    int y;
}

int cat_age(const Cat c) {
    return c.get();
}

int point_x(const Point p) {
    return p.x;
}

int scalar(const int n) {
    return n + 1;
}

int deref(const int* p) {
    return p[0];
}

int main() {
    Cat c = new Cat();
    Point p = {1, 2};
    int x = 3;
    int y = cat_age(c) + point_x(p) + scalar(x) + deref(&x);
    return y;
}
