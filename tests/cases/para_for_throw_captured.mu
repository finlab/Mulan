unit para_for_throw_captured;

// A throwing source under para for with a captured string: the region must join every task before
// the frame unwinds and releases the captured buffer (no use-after-free in the body).

use std.cstdio;
use std.thread;
use std.task;
use std.string;

struct Cursor {
    public int i;
    public int n;
    public int broken;
    public bool next() {
        i = i + 1;
        if i == broken {
            throw new Exception("source failed");
        }
        return i <= n;
    }
    public int current() { return i; }
}

struct Source {
    public int n;
    public int broken;
    public Cursor iterator() {
        Cursor c;
        c.i = 0;
        c.n = n;
        c.broken = broken;
        return c;
    }
}

int main() {
    set_max_live_tasks(200);
    int caught = 0;
    for r in 0..20 {
        try {
            string tag = "captured buffer read by the body";
            Source s;
            s.n = 50;
            s.broken = 45;
            para for v in s {
                thread_sleep(1);
                if tag.length() == 0 { }
            }
        } catch (e: TaskLimitError) {
            printf("limit\n");
            return 2;
        } catch (e: Exception) {
            caught = caught + 1;
        }
    }
    printf("%d\n", caught);
    return 0;
}
