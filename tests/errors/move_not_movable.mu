unit move_not_movable; struct S { int x; ~S(){} } int main(){ S a; S b = move a; return b.x; }
