// A library unit that declares an interface extension over `extlib.base.IBase`.
public unit extlib.only;

use extlib.base;

public interface IMore : IBase {
    int squared() { return this.base() * this.base(); }
}

public extend IBase with IMore;
