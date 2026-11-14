// para_limit_sections — a failed start in a static `para` is an ordinary exception: the sections
// that already started are joined before the region unwinds, so no task is left behind and the live
// count returns to zero.

unit para_limit_sections;

use std.cstdio;
use std.thread;
use std.task;

int main() {
    set_max_live_tasks(2);
    try {
        para {
            { thread_sleep(30); }
            { thread_sleep(30); }
            { thread_sleep(30); }
        }
        printf("no error\n");
    } catch (e: TaskLimitError) {
        printf("limit %d\n", live_task_count());
    }
    return 0;
}
