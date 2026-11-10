unit unit_name_var;

// 局部/全局变量与 unit 同名时，`v.field = x` 仍必须是合法左值
// （is_lvalue 不能因为名字碰巧等于 unit 名就把它当成 unit 限定访问）。

extern int puts(const char* s);

struct S { int x; }

S unit_name_var = {1};

int main() {
    unit_name_var.x = 7;
    if unit_name_var.x != 7 { puts("global field assign failed"); return 1; }

    S unit_name_var = {2};  // 局部遮蔽同名全局
    unit_name_var.x = 9;
    if unit_name_var.x != 9 { puts("local field assign failed"); return 2; }

    puts("unit name var ok");
    return 0;
}
