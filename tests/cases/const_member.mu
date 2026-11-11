// const_member.mu — 类型级编译期常量（struct / class 体内的 `const`）
//
// 覆盖：标量各类型、常量之间互相引用、sizeof/alignof、`Type.Name`、方法体内裸名、
// 基类常量（`Derived.Name` 与派生类裸名）、private 同 unit 可读、unit 级 const
// 引用类型级常量、以及常量成员不占实例内存（sizeof 不变）。
unit const_member;

extern int puts(const char* s);

enum Mood { Calm, Happy }

struct Plain {
    int a;
}

struct Cat {
    const int MaxAge = 30;
    const uint Limit = 40;
    const byte Small = 200;
    const bool Cute = true;
    const float Weight = 1.5;
    const double Ratio = 2.5;
    const Mood DefaultMood = Mood.Happy;
    const int Twice = MaxAge * 2;          // 引用同类型常量
    const int Size = sizeof(int) as int;   // sizeof(...) as int
    const int Align = alignof(int) as int;
    const int Neg = -MaxAge;               // 一元运算
    const bool Big = MaxAge > 20;          // 比较
    const short ShortVal = 300;            // 各整数位宽（负字面量窄化与运行期一样不被接受）
    const ushort UShortVal = 60000;
    const long LongVal = 5000000000;
    const isize ISizeVal = -456;
    const usize USizeVal = 123;
    const char CharVal = '\u{e9}';         // 字符字面量按位模式适配（运行期 char 为有符号 8 位）
    const ulong ULongShift = ((0x4000000000000000 as ulong) << 1) >> 60;  // 无符号 >> 与运行期一致
    const uint UIntShift = (0x80000000 as uint) >> 4;
    private const int Secret = 7;          // private：本 unit 内可读
    int age;

    int age_plus_max() { return age + MaxAge; }  // 方法体内裸名

    int shadow() {                         // 局部变量遮蔽同名常量
        int MaxAge = 5;
        return MaxAge;
    }
}

class Animal {
    const int Legs = 4;
    const int Eyes = Legs / 2;
    int legs() { return Legs; }
}

class Dog : Animal {
    const int LegsTimesTwo = Legs * 2;     // 派生类裸名访问基类常量
    const int BaseEyes = Animal.Eyes;      // 通过基类名访问
    int dog_legs() { return Legs; }
}

// unit 级 const 引用类型级常量
const int GlobalFromMember = Cat.MaxAge + Cat.Size;

int main() {
    // ---------- 各标量类型 ----------
    if Cat.MaxAge != 30 { puts("int failed"); return 1; }
    if Cat.Limit != 40 { puts("uint failed"); return 1; }
    if Cat.Small != 200 { puts("byte failed"); return 1; }
    if not Cat.Cute { puts("bool failed"); return 1; }
    if Cat.Weight != 1.5 { puts("float failed"); return 1; }
    if Cat.Ratio != 2.5 { puts("double failed"); return 1; }
    if Cat.DefaultMood != Mood.Happy { puts("enum failed"); return 1; }
    puts("const member basics ok");

    // ---------- 常量表达式 ----------
    if Cat.Twice != 60 { puts("reference failed"); return 1; }
    if Cat.Size != 4 { puts("sizeof failed"); return 1; }
    if Cat.Align != 4 { puts("alignof failed"); return 1; }
    if Cat.Neg != -30 { puts("unary failed"); return 1; }
    if not Cat.Big { puts("compare failed"); return 1; }
    if Cat.Secret != 7 { puts("private same-unit failed"); return 1; }
    puts("const member expressions ok");

    // ---------- 更多整数位宽 / 字符字面量 ----------
    if Cat.ShortVal != 300 { puts("short failed"); return 1; }
    if Cat.UShortVal != 60000 { puts("ushort failed"); return 1; }
    if Cat.LongVal != 5000000000 { puts("long failed"); return 1; }
    if Cat.ISizeVal != -456 { puts("isize failed"); return 1; }
    if Cat.USizeVal != 123 { puts("usize failed"); return 1; }
    if Cat.CharVal != -23 { puts("char literal member failed"); return 1; }
    puts("const member wide types ok");

    // ---------- 无符号 >>：常量结果必须与运行期一致 ----------
    ulong x = (0x4000000000000000 as ulong) << 1;
    uint u = 0x80000000 as uint;
    if Cat.ULongShift != x >> 60 { puts("ulong shr mismatch"); return 1; }
    if Cat.UIntShift != u >> 4 { puts("uint shr mismatch"); return 1; }
    if Cat.ULongShift as long != -8 { puts("ulong shr const wrong"); return 1; }
    if Cat.UIntShift != 4160749568 { puts("uint shr const wrong"); return 1; }
    puts("const member shr ok");

    // ---------- 方法体内裸名 + 与实例字段混用 ----------
    Cat c;
    c.age = 12;
    if c.age_plus_max() != 42 { puts("bare name in method failed"); return 1; }
    puts("const member methods ok");

    // ---------- 局部变量遮蔽同名常量 ----------
    if c.shadow() != 5 { puts("local shadow failed"); return 1; }
    if Cat.MaxAge != 30 { puts("const changed by shadow"); return 1; }
    puts("const member shadow ok");

    // ---------- 继承 ----------
    if Animal.Legs != 4 { puts("base const failed"); return 1; }
    if Dog.Legs != 4 { puts("inherited const failed"); return 1; }
    if Dog.LegsTimesTwo != 8 { puts("derived bare name failed"); return 1; }
    if Dog.BaseEyes != 2 { puts("base qualified failed"); return 1; }
    Animal a = new Animal();
    if a.legs() != 4 { puts("base method failed"); return 1; }
    a = null;
    Dog d = new Dog();
    if d.dog_legs() != 4 { puts("derived method failed"); return 1; }
    d = null;
    puts("const member inheritance ok");

    // ---------- 常量不占实例内存 ----------
    if sizeof(Cat) != sizeof(Plain) { puts("layout changed"); return 1; }
    puts("const member layout ok");

    // ---------- unit 级 const ----------
    if GlobalFromMember != 34 { puts("unit const from member failed"); return 1; }
    puts("const member unit-level ok");

    return 0;
}
