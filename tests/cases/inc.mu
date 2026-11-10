unit inc;
extern int puts(const char* s);

int main() {
    int i = 0;
    i++;
    i++;
    ++i;
    if i != 3 { puts("inc fail"); return 1; }

    int j = 0;
    j++; j++;
    if j != 2 { puts("inc same line fail"); return 2; }

    int k = 5;
    --k;
    if k != 4 { puts("dec fail"); return 3; }

    int* p = &i;
    (*p)++;
    if i != 4 { puts("ptr inc fail"); return 4; }
    *p++;
    if i != 5 { puts("deref suffix inc fail"); return 5; }

    puts("inc ok");
    return 0;
}
