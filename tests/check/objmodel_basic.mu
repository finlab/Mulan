unit check_objmodel;

// object model basics: struct operators, class construction/destruction, methods, operator[]/[]=, function overloading

struct Point { int x, y; }

Point operator+(Point a, Point b) { return Point(a.x + b.x, a.y + b.y); }
bool operator==(Point a, Point b) { return a.x == b.x and a.y == b.y; }

class Vec {
    int[3] data;
    Vec() { data[0] = 0; data[1] = 0; data[2] = 0; }
    ~Vec() { }
    int get(int i) { return data[i]; }
}

int operator[](Vec v, int i) { return v.get(i); }
void operator[]=(Vec v, int i, int x) { v.data[i] = x; }

int f(int a) { return 1; }
int f(long a) { return 2; }
int f(string s) { return 3; }

int main() {
    Point p = {1, 2};
    Point q = {3, 4};
    Point r = p + q;
    if r == Point(4, 6) { }

    Vec v = new Vec();
    v[0] = 7;
    int x = v[0];
    v = null;

    int n = f(1);
    long big = 1;
    int m = f(big);
    int k = f("x");
    return n + m + k + x;
}
