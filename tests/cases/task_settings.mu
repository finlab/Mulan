unit task_settings;
use std.cstdio;
use std.exception;
use std.task;

// The runtime settings reject values they cannot apply instead of silently keeping the old ones.
// `set_max_live_tasks(0)` is the documented kill switch (no further task starts).
int main() {
    int caught = 0;
    try {
        set_max_live_tasks(-1);
    } catch (e: Exception) {
        caught += 1;
    }
    try {
        set_task_stack(0, 0);
    } catch (e: Exception) {
        caught += 1;
    }
    try {
        set_task_stack(100, 50);
    } catch (e: Exception) {
        caught += 1;
    }
    set_max_live_tasks(1000);
    set_task_stack(65536, 262144);
    printf("caught=%d\n", caught);
    return 0;
}
