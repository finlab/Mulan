unit class_cleanup;

extern int puts(const char* s);

int live = 0;

class C {
    int v;
    C() { v = 0; live += 1; }
    ~C() { live -= 1; }
}

int make_one() {
    C c = new C();      // 没有置 null，返回时由清理逻辑释放
    return 1;
}

void take(C c) {
    if c.v != 0 { }
}

int main() {
    {
        C a = new C();
        if live != 1 { puts("block live failed"); return 1; }
    }
    if live != 0 { puts("block cleanup failed"); return 1; }

    int r = make_one();
    if r != 1 { puts("make failed"); return 1; }
    if live != 0 { puts("return cleanup failed"); return 1; }

    for i in 0..3 {
        C l = new C();
    }
    if live != 0 { puts("for cleanup failed"); return 1; }

    int i = 0;
    while i < 3 {
        C l = new C();
        i += 1;
    }
    if live != 0 { puts("while cleanup failed"); return 1; }

    while true {
        C l = new C();
        if live != 1 { puts("break live failed"); return 1; }
        break;
    }
    if live != 0 { puts("break cleanup failed"); return 1; }

    int j = 0;
    while j < 3 {
        j += 1;
        C l = new C();
        continue;
    }
    if live != 0 { puts("continue cleanup failed"); return 1; }

    {
        C a = new C();
        C b = a;        // 共享引用
        a = null;
        if live != 1 { puts("share live failed"); return 1; }
        if b.v != 0 { puts("share value failed"); return 1; }
    }
    if live != 0 { puts("share cleanup failed"); return 1; }

    take(new C());              // 临时实参应在调用后释放
    if live != 0 { puts("temp arg cleanup failed"); return 1; }

    int q = (new C()).v;        // 临时接收者的成员访问
    if q != 0 { puts("temp member failed"); return 1; }
    if live != 0 { puts("temp member cleanup failed"); return 1; }

    new C();                    // 表达式语句丢弃临时对象
    if live != 0 { puts("temp stmt cleanup failed"); return 1; }

    puts("class cleanup ok");
    return 0;
}
