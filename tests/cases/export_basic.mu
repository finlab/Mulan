// export_basic.mu — `export` implies `public`: exported declarations are usable across units
unit export_basic;

use exportlib.api;

extern int puts(const char* s);

int main() {
    if twice(21) != 42 { puts("export fn failed"); return 1; }
    Shape s = new Shape(3);
    if s.area() != 9 { puts("export class failed"); return 1; }
    puts("export ok");
    return 0;
}
