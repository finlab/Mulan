// A 65-edge interface chain: the constraint check must reach the requirement at its end (#103).
unit iface_constraint_deep;

interface I0 : I1 {}
interface I1 : I2 {}
interface I2 : I3 {}
interface I3 : I4 {}
interface I4 : I5 {}
interface I5 : I6 {}
interface I6 : I7 {}
interface I7 : I8 {}
interface I8 : I9 {}
interface I9 : I10 {}
interface I10 : I11 {}
interface I11 : I12 {}
interface I12 : I13 {}
interface I13 : I14 {}
interface I14 : I15 {}
interface I15 : I16 {}
interface I16 : I17 {}
interface I17 : I18 {}
interface I18 : I19 {}
interface I19 : I20 {}
interface I20 : I21 {}
interface I21 : I22 {}
interface I22 : I23 {}
interface I23 : I24 {}
interface I24 : I25 {}
interface I25 : I26 {}
interface I26 : I27 {}
interface I27 : I28 {}
interface I28 : I29 {}
interface I29 : I30 {}
interface I30 : I31 {}
interface I31 : I32 {}
interface I32 : I33 {}
interface I33 : I34 {}
interface I34 : I35 {}
interface I35 : I36 {}
interface I36 : I37 {}
interface I37 : I38 {}
interface I38 : I39 {}
interface I39 : I40 {}
interface I40 : I41 {}
interface I41 : I42 {}
interface I42 : I43 {}
interface I43 : I44 {}
interface I44 : I45 {}
interface I45 : I46 {}
interface I46 : I47 {}
interface I47 : I48 {}
interface I48 : I49 {}
interface I49 : I50 {}
interface I50 : I51 {}
interface I51 : I52 {}
interface I52 : I53 {}
interface I53 : I54 {}
interface I54 : I55 {}
interface I55 : I56 {}
interface I56 : I57 {}
interface I57 : I58 {}
interface I58 : I59 {}
interface I59 : I60 {}
interface I60 : I61 {}
interface I61 : I62 {}
interface I62 : I63 {}
interface I63 : I64 {}
interface I64 : I65 {}
interface I65 { int required(); }

struct Bad {}

void require<T: I0>(T value) {}

int main() { Bad b; require<Bad>(b); return 0; }
