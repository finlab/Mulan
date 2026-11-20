// An interface member is an itable slot keyed by the value signature: `ref` is not available there.
unit ref_iface_param;
interface I {
    void set(ref int x);
}
int main() { return 0; }
