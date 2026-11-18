// A method declared 65 classes up satisfies a shallow interface constraint (#103).
unit iface_constraint_deep_base;

interface I { int required(); }

class C65 { public int required() { return 1; } }
class C64 : C65 {}
class C63 : C64 {}
class C62 : C63 {}
class C61 : C62 {}
class C60 : C61 {}
class C59 : C60 {}
class C58 : C59 {}
class C57 : C58 {}
class C56 : C57 {}
class C55 : C56 {}
class C54 : C55 {}
class C53 : C54 {}
class C52 : C53 {}
class C51 : C52 {}
class C50 : C51 {}
class C49 : C50 {}
class C48 : C49 {}
class C47 : C48 {}
class C46 : C47 {}
class C45 : C46 {}
class C44 : C45 {}
class C43 : C44 {}
class C42 : C43 {}
class C41 : C42 {}
class C40 : C41 {}
class C39 : C40 {}
class C38 : C39 {}
class C37 : C38 {}
class C36 : C37 {}
class C35 : C36 {}
class C34 : C35 {}
class C33 : C34 {}
class C32 : C33 {}
class C31 : C32 {}
class C30 : C31 {}
class C29 : C30 {}
class C28 : C29 {}
class C27 : C28 {}
class C26 : C27 {}
class C25 : C26 {}
class C24 : C25 {}
class C23 : C24 {}
class C22 : C23 {}
class C21 : C22 {}
class C20 : C21 {}
class C19 : C20 {}
class C18 : C19 {}
class C17 : C18 {}
class C16 : C17 {}
class C15 : C16 {}
class C14 : C15 {}
class C13 : C14 {}
class C12 : C13 {}
class C11 : C12 {}
class C10 : C11 {}
class C9 : C10 {}
class C8 : C9 {}
class C7 : C8 {}
class C6 : C7 {}
class C5 : C6 {}
class C4 : C5 {}
class C3 : C4 {}
class C2 : C3 {}
class C1 : C2 {}
class C0 : C1 {}

void require<T: I>(T value) {}

int main() { C0 c = new C0(); require<C0>(c); return 0; }
