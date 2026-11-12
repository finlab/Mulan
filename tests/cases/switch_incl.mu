// switch_incl.mu — switch 分支区间支持 `..=`（含上界）
unit switch_incl;

extern int puts(const char* s);

int classify(int v) {
    switch v {
        0..=9: {
            return 1;
        }
        10..=99: {
            return 2;
        }
        else: {
            return 3;
        }
    }
}

int main() {
    if classify(0) == 1 and classify(9) == 1 and classify(10) == 2 and classify(99) == 2 and classify(100) == 3 {
        puts("switch inclusive ok");
    }
    return 0;
}
