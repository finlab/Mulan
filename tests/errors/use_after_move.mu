unit use_after_move; nocopy struct Buf { int x; Buf() { x = 1; } Buf(move Buf o) { x = o.x; } } int main(){ Buf a; Buf b = move a; int v = a.x; return v; }
