// para_for_limit — the scheduler caps live execution units and throws (SPEC 3.17.7). The cap is a
// runtime setting, so the test lowers it; a source that produces items faster than the tasks retire
// hits it, the region joins what already started, and the refusal surfaces as an ordinary exception
// at the region's join point.

unit para_for_limit;

use std.cstdio;
use std.thread;
use std.task;

int main() {
    set_max_live_tasks(100);
    try {
        para for i in 0..4000 {
            thread_sleep(200);
        }
        printf("no error\n");
    } catch (e: TaskLimitError) {
        printf("limit\n");
    }
    return 0;
}
