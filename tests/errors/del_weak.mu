unit del_weak;
class N { int v; weak N next; N(){v=0;next=null;} ~N(){} }
int main() { N a = new N(); N b = new N(); a.next = b; del a.next; del b; del a; return 0; }
