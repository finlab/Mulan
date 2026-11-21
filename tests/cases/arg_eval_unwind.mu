// arg_eval_unwind — a later argument throwing releases the arguments already evaluated (#216).
unit arg_eval_unwind;

extern int puts(const char* s);

int live = 0;
class Res { Res() { live += 1; } ~Res() { live -= 1; } }
Res mk() { return new Res(); }
int boom() { throw new Exception("boom"); return 0; }
int sink(Res a, int b) { return 0; }

int main() {
    try { sink(mk(), boom()); } catch (e: Exception) { }
    if live != 0 { puts("argument leak"); return 1; }
    puts("arg eval unwind ok");
    return 0;
}
