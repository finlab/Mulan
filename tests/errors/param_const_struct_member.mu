unit pe4;

struct Point {
    int x;
    int y;
}

void move_x(const Point p) {
    p.x = 1;
}

int main() {
    Point p = {2, 3};
    move_x(p);
    return 0;
}
