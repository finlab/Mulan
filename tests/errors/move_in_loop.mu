unit move_in_loop; struct P { int x; } int main(){ P a; for i in 0..3 { P b = move a; } return 0; }
