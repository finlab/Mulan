// const_member.mu — type-level compile-time constants (`const` inside struct / class bodies)
//
// Covers: every scalar type, references between constants, sizeof/alignof, `Type.Name`, bare names in method
// bodies, base class constants (`Derived.Name` and the bare name in a derived class), private readable in the
// same unit, unit-level const referencing a type-level constant, and constants taking no instance memory (sizeof unchanged).
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
    const int Twice = MaxAge * 2;          // references a constant of the same type
    const int Size = sizeof(int) as int;   // sizeof(...) as int
    const int Align = alignof(int) as int;
    const int Neg = -MaxAge;               // unary operator
    const bool Big = MaxAge > 20;          // comparison
    const short ShortVal = 300;            // integer widths (narrowing a negative literal is rejected, same as at run time)
    const ushort UShortVal = 60000;
    const long LongVal = 5000000000;
    const isize ISizeVal = -456;
    const usize USizeVal = 123;
    const char CharVal = '\u{e9}';         // char literal adapts by bit pattern (char is a signed 8-bit type at run time)
    const ulong ULongShift = ((0x4000000000000000 as ulong) << 1) >> 60;  // unsigned >> matches run time
    const uint UIntShift = (0x80000000 as uint) >> 4;
    private const int Secret = 7;          // private: readable inside this unit
    int age;

    int age_plus_max() { return age + MaxAge; }  // bare name inside a method body

    int shadow() {                         // a local variable shadows the constant of the same name
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
    const int LegsTimesTwo = Legs * 2;     // derived class reaches the base constant by bare name
    const int BaseEyes = Animal.Eyes;      // accessed through the base class name
    int dog_legs() { return Legs; }
}

// unit-level const referencing a type-level constant
const int GlobalFromMember = Cat.MaxAge + Cat.Size;

int main() {
    // ---------- scalar types ----------
    if Cat.MaxAge != 30 { puts("int failed"); return 1; }
    if Cat.Limit != 40 { puts("uint failed"); return 1; }
    if Cat.Small != 200 { puts("byte failed"); return 1; }
    if not Cat.Cute { puts("bool failed"); return 1; }
    if Cat.Weight != 1.5 { puts("float failed"); return 1; }
    if Cat.Ratio != 2.5 { puts("double failed"); return 1; }
    if Cat.DefaultMood != Mood.Happy { puts("enum failed"); return 1; }
    puts("const member basics ok");

    // ---------- constant expressions ----------
    if Cat.Twice != 60 { puts("reference failed"); return 1; }
    if Cat.Size != 4 { puts("sizeof failed"); return 1; }
    if Cat.Align != 4 { puts("alignof failed"); return 1; }
    if Cat.Neg != -30 { puts("unary failed"); return 1; }
    if not Cat.Big { puts("compare failed"); return 1; }
    if Cat.Secret != 7 { puts("private same-unit failed"); return 1; }
    puts("const member expressions ok");

    // ---------- more integer widths / char literal ----------
    if Cat.ShortVal != 300 { puts("short failed"); return 1; }
    if Cat.UShortVal != 60000 { puts("ushort failed"); return 1; }
    if Cat.LongVal != 5000000000 { puts("long failed"); return 1; }
    if Cat.ISizeVal != -456 { puts("isize failed"); return 1; }
    if Cat.USizeVal != 123 { puts("usize failed"); return 1; }
    if Cat.CharVal != -23 { puts("char literal member failed"); return 1; }
    puts("const member wide types ok");

    // ---------- unsigned >>: the constant result must match run time ----------
    ulong x = (0x4000000000000000 as ulong) << 1;
    uint u = 0x80000000 as uint;
    if Cat.ULongShift != x >> 60 { puts("ulong shr mismatch"); return 1; }
    if Cat.UIntShift != u >> 4 { puts("uint shr mismatch"); return 1; }
    if Cat.ULongShift as long != -8 { puts("ulong shr const wrong"); return 1; }
    if Cat.UIntShift != 4160749568 { puts("uint shr const wrong"); return 1; }
    puts("const member shr ok");

    // ---------- bare name in a method body + mixed with instance fields ----------
    Cat c;
    c.age = 12;
    if c.age_plus_max() != 42 { puts("bare name in method failed"); return 1; }
    puts("const member methods ok");

    // ---------- a local variable shadows the constant of the same name ----------
    if c.shadow() != 5 { puts("local shadow failed"); return 1; }
    if Cat.MaxAge != 30 { puts("const changed by shadow"); return 1; }
    puts("const member shadow ok");

    // ---------- inheritance ----------
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

    // ---------- constants take no instance memory ----------
    if sizeof(Cat) != sizeof(Plain) { puts("layout changed"); return 1; }
    puts("const member layout ok");

    // ---------- unit-level const ----------
    if GlobalFromMember != 34 { puts("unit const from member failed"); return 1; }
    puts("const member unit-level ok");

    return 0;
}
