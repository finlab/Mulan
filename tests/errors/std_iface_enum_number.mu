unit std_iface_enum_number;
use std.interfaces;
enum Color { Red, Green }
T twice<T: INumber>(T a) { return a + a; }
int main() {
    return twice<Color>(Color.Red) as int;
}
