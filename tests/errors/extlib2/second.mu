public unit extlib2.second;

use extlib2.first;

public interface JSecond : I { int g() { return 2; } }

public extend I with JSecond;
