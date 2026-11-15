// `>>` on an unsigned type must be a logical shift, performed at the declared width (`byte >> 1` is
// an i8 shift), and the constant folder must agree with the instruction the backend emits. Signed
// types keep the arithmetic shift. Regression for issue #17, where every unsigned width used AShr.
unit shift_unsigned;

extern int printf(const char* fmt, ...);

// Folded at compile time: these go through the constant path, not the backend.
const uint  CU = 0x8B0B0000;      // top bit set
const int   CI = -1000;
const ulong CL = (1 as ulong) << 63;
const byte  CB = 200;

int main() {
    int si = -1000;
    byte b = 200;
    ushort s = 40000;
    uint u = 0x8B0B0000;
    ulong l = (1 as ulong) << 63;
    printf("runtime int=%d byte=%u ushort=%u uint=%u ulong=%llu\n",
           si >> 3, (b >> 1) as uint, (s >> 8) as uint, u >> 16, l >> 4);

    uint cu = 0x8B0B0000;
    cu >>= 16;
    ulong cl = (1 as ulong) << 63;
    cl >>= 60;
    int si2 = -64;
    si2 >>= 2;
    printf("compound uint=%u ulong=%llu int=%d\n", cu, cl, si2);

    printf("const uint=%u int=%d ulong=%llu byte=%u\n",
           CU >> 16, CI >> 3, (CL >> 4) as ulong, (CB >> 1) as uint);
    return 0;
}
