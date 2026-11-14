unit delegate_extern;
type Op = delegate (int, int) => int;

extern void register_callback(Op op);

int main() {
    return 0;
}
