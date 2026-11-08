unit check_unary_op;

// 一元运算符重载（operator- / operator+）+ 比较运算符

struct Vec2 { int x, y; }

Vec2 operator-(Vec2 a) { return Vec2(-a.x, -a.y); }
Vec2 operator+(Vec2 a) { return Vec2(a.x, a.y); }
bool operator==(Vec2 a, Vec2 b) { return a.x == b.x and a.y == b.y; }
bool operator!=(Vec2 a, Vec2 b) { return not (a == b); }

int main() {
    Vec2 a = {1, 2};
    Vec2 b = -a;
    Vec2 c = +a;
    if b != c { return 1; }
    return 0;
}
