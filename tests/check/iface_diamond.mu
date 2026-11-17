// A diamond re-visits its shared ancestors, so the depth guard must measure the longest path, not
// the number of visits: 40 diamond levels contain 80 interfaces but no chain longer than 40.
unit iface_diamond;

interface A0 {}
interface B0 {}
interface A1 : A0, B0 {}
interface B1 : A0, B0 {}
interface A2 : A1, B1 {}
interface B2 : A1, B1 {}
interface A3 : A2, B2 {}
interface B3 : A2, B2 {}
interface A4 : A3, B3 {}
interface B4 : A3, B3 {}
interface A5 : A4, B4 {}
interface B5 : A4, B4 {}
interface A6 : A5, B5 {}
interface B6 : A5, B5 {}
interface A7 : A6, B6 {}
interface B7 : A6, B6 {}
interface A8 : A7, B7 {}
interface B8 : A7, B7 {}
interface A9 : A8, B8 {}
interface B9 : A8, B8 {}
interface A10 : A9, B9 {}
interface B10 : A9, B9 {}
interface A11 : A10, B10 {}
interface B11 : A10, B10 {}
interface A12 : A11, B11 {}
interface B12 : A11, B11 {}
interface A13 : A12, B12 {}
interface B13 : A12, B12 {}
interface A14 : A13, B13 {}
interface B14 : A13, B13 {}
interface A15 : A14, B14 {}
interface B15 : A14, B14 {}
interface A16 : A15, B15 {}
interface B16 : A15, B15 {}
interface A17 : A16, B16 {}
interface B17 : A16, B16 {}
interface A18 : A17, B17 {}
interface B18 : A17, B17 {}
interface A19 : A18, B18 {}
interface B19 : A18, B18 {}
interface A20 : A19, B19 {}
interface B20 : A19, B19 {}
interface A21 : A20, B20 {}
interface B21 : A20, B20 {}
interface A22 : A21, B21 {}
interface B22 : A21, B21 {}
interface A23 : A22, B22 {}
interface B23 : A22, B22 {}
interface A24 : A23, B23 {}
interface B24 : A23, B23 {}
interface A25 : A24, B24 {}
interface B25 : A24, B24 {}
interface A26 : A25, B25 {}
interface B26 : A25, B25 {}
interface A27 : A26, B26 {}
interface B27 : A26, B26 {}
interface A28 : A27, B27 {}
interface B28 : A27, B27 {}
interface A29 : A28, B28 {}
interface B29 : A28, B28 {}
interface A30 : A29, B29 {}
interface B30 : A29, B29 {}
interface A31 : A30, B30 {}
interface B31 : A30, B30 {}
interface A32 : A31, B31 {}
interface B32 : A31, B31 {}
interface A33 : A32, B32 {}
interface B33 : A32, B32 {}
interface A34 : A33, B33 {}
interface B34 : A33, B33 {}
interface A35 : A34, B34 {}
interface B35 : A34, B34 {}
interface A36 : A35, B35 {}
interface B36 : A35, B35 {}
interface A37 : A36, B36 {}
interface B37 : A36, B36 {}
interface A38 : A37, B37 {}
interface B38 : A37, B37 {}
interface A39 : A38, B38 {}
interface B39 : A38, B38 {}

struct S : A39 { }

int main() { return 0; }
