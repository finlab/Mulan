unit generic_abstract_new;
class A<T> { public T v; A() {} T f(); }
int main() { A<int> a = new A<int>(); return 0; }
