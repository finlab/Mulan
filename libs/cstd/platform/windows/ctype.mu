// cstd.ctype — <ctype.h> for Windows (UCRT / MSVCRT, built with mingw-w64).
//
// Raw binding (layer L1): the C functions, not the macro forms. Behaviour is locale-dependent and
// undefined for arguments outside the unsigned-char range (and EOF).

public unit cstd.ctype;

use std.ctypes;

extern int isalnum(int c);
extern int isalpha(int c);
extern int isblank(int c);
extern int iscntrl(int c);
extern int isdigit(int c);
extern int isgraph(int c);
extern int islower(int c);
extern int isprint(int c);
extern int ispunct(int c);
extern int isspace(int c);
extern int isupper(int c);
extern int isxdigit(int c);

extern int tolower(int c);
extern int toupper(int c);
