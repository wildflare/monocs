// a.c と一緒にリンクする
static int f(void) { return 2; }
static int counter_a = 200;                  // a.c と同じ名前の static
int g = 20;
int h;
int counter = 7;
int a_twice(int);
static int twice_plus(int x) { return a_twice(x) + counter_a; }
int (*b_ptr)(int) = twice_plus;
int b_func(int x) { h = 3; return x * 100 + f(); }
const char *b_name(void) { static const char name[] = "b.c"; return name; }
