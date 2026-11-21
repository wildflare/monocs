// 複数のオブジェクトのリンク（slimcc_link_many）の確認: ファイルごとの static、弱い定義と強い定義、
// ほかのファイルの関数・変数・関数ポインタ。b.c と一緒にコンパイルし、GCC でリンクしたものと出力を比べる
#include <stdio.h>
static int f(void) { return 1; }            // b.c にも同じ名前の static がある
static int counter_a = 100;
int g __attribute__((weak)) = 10;            // b.c の強い定義が勝つ
int only_weak __attribute__((weak)) = 5;     // 弱い定義しかない
extern int h, counter;
extern int (*b_ptr)(int);
int b_func(int);
const char *b_name(void);
int a_twice(int x) { return x * 2; }         // b.c から呼ばれる
int main(int argc, char **argv) {
    printf("f=%d counter_a=%d g=%d only_weak=%d\n", f(), counter_a, g, only_weak);
    printf("b_func=%d h=%d counter=%d b_ptr=%d name=%s\n", b_func(5), h, counter, b_ptr(7), b_name());
    printf("argc=%d argv1=%s\n", argc, argc > 1 ? argv[1] : "-");
    return 0;
}
