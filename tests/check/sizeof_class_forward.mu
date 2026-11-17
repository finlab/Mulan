// A class is a reference, so `sizeof` it is the handle size and does not need its fields. Resolving
// them for `sizeof` made a field that names a later class fail (and crashed when the class was not
// indexed yet).
unit sizeof_class_forward;

class First { Later value; }
class Later { int n; }

const int SIZE = sizeof(First) as int;

int main() { return 0; }
