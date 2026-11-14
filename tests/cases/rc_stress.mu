unit rc_stress;

use std.cstdio;
use std.thread;

extern void* calloc(usize n, usize size);

const int ROUNDS = 50000;

Mutex g_m;
CondVar g_has_item;
CondVar g_has_space;
Mutex g_destroy_lock;

Slot* g_slot = null;
bool g_closed = false;
int g_destroyed = 0;

// The destructor runs on the thread that releases the last reference -- here always the
// consumer, never the producer that created the object.
class Node {
    public int v;
    ~Node() {
        mutex_lock(g_destroy_lock);
        g_destroyed = g_destroyed + 1;
        mutex_unlock(g_destroy_lock);
    }
}

// A zeroed Slot holds a null class field, so `(*s).n = ...` releases nothing on first store.
struct Slot { Node n; }

void producer(void* arg) {
    for i in 0..ROUNDS {
        Slot* s = calloc(1, sizeof(Slot)) as Slot*;
        (*s).n = new Node();
        mutex_lock(g_m);
        while g_slot != null {
            cond_wait(g_has_space, g_m, WAIT_INFINITE);
        }
        g_slot = s;
        cond_signal(g_has_item);
        mutex_unlock(g_m);
    }
    mutex_lock(g_m);
    g_closed = true;
    cond_signal(g_has_item);
    mutex_unlock(g_m);
}

void consumer(void* arg) {
    while true {
        mutex_lock(g_m);
        while g_slot == null and not g_closed {
            cond_wait(g_has_item, g_m, WAIT_INFINITE);
        }
        if g_slot == null and g_closed {
            mutex_unlock(g_m);
            break;
        }
        Slot* s = g_slot;
        g_slot = null;
        cond_signal(g_has_space);
        mutex_unlock(g_m);

        Node n = (*s).n;      // retain on this thread
        (*s).n = null;        // drop the slot's reference
        free(s);
        n.v = n.v + 1;        // local release at the end of the iteration -> destructor here
    }
}

int main() {
    Thread p = thread_start(producer, null);
    Thread c = thread_start(consumer, null);
    thread_join(p);
    thread_join(c);
    printf("%d %d\n", g_destroyed, ROUNDS);
    return 0;
}
