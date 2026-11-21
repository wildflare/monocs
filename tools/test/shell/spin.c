// 終わらないアプリ（timeout の試験）
int main(void)
{
    for (volatile int i = 0;; i++)
        ;
}
