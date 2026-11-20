// string_range_errors — repeat overflow, unicode_copy_range out of range, and ByteCursor.current
// before next() are diagnosed instead of wrapping or returning padding (#229, #234, #236).
unit string_range_errors;

use std.string;
use std.unicode;

extern int puts(const char* s);

int main() {
    int caught = 0;
    {
        string s = "abc";
        try {
            string r = s.repeat(6148914691236517206);
        } catch (e: Exception) {
            caught += 1;
        }
    }
    {
        string h = "hello";
        try {
            string t = unicode_copy_range(h, 0, 100);
        } catch (e: Exception) {
            caught += 1;
        }
    }
    {
        ByteCursor bc = ByteCursor("ab");
        try {
            byte x = bc.current();
        } catch (e: Exception) {
            caught += 1;
        }
    }
    if caught != 3 { puts("range errors"); return 1; }
    puts("string range errors ok");
    return 0;
}
