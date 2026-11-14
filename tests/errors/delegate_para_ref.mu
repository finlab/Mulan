unit delegate_para_ref;
type Bump = delegate () => void;

int main() {
    int n = 0;
    para {
        { Bump b = [ref n] () => { n = n + 1; }; b(); }
    }
    return n;
}
