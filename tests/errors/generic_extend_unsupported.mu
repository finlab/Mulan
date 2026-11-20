unit generic_extend_unsupported;
interface IBase<T> { T get(); }
interface IExt<T> : IBase<T> { T twice() { return get(); } }
extend IBase<T> with IExt<T>;
int main() { return 0; }
