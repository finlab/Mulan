unit generic_weak_struct; class Node { public int id; } struct Box<T> { weak Node peer; T payload; } int main() { Box<int> b; return 0; }
