unit base_ctor_visibility;
use basevis.base;
public class D : Base {
    D() : Base(1) { }
}
int main() { D d = new D(); return 0; }
