// NULL に書いて例外で止まる（$? が 139、dmesg に記録が残ること）
int main(void)
{
    *(volatile int *)0 = 1;
    return 0;
}
