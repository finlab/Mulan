// Delegating constructors are not part of the language yet: the diagnostic must name the form instead of
// reporting the class name as an unknown field.
unit ctor_delegating;

class Cfg {
    public const int id;
    Cfg(int i) : id(i) { }
    Cfg() : Cfg(7) { }
}

int main() { return 0; }
