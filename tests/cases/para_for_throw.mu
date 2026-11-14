// para_for_throw — an exception out of the source (a throwing `next()` mid-stream) leaves the region
// cleanly: the producer joins every task it already started, so neither the live-task count nor the
// captured slots are left dangling. Each round starts 44 tasks and fails on item 45; a leaked count
// would exhaust the 1000-task cap long before the last round and surface as TaskLimitError.

unit para_for_throw;

use std.cstdio;
use std.thread;
use std.task;

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
    // A leaked join would leave 44 live tasks per round; the cap makes that visible long before the
    // last round instead of only exhausting memory.
    set_max_live_tasks(200);
    int caught = 0;
    int limited = 0;
    for r in 0..30 {
        try {
            Source s;
            s.n = 50;
            s.broken = 45;
            para for v in s {
                thread_sleep(1);
            }
        } catch (e: TaskLimitError) {
            limited = limited + 1;
        } catch (e: Exception) {
            caught = caught + 1;
        }
    }
    printf("%d %d\n", caught, limited);
    return 0;
}
