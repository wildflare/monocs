#include <math.h>
#include <stdio.h>

int main(void)
{
    for (int i = 0; i <= 8; i++) {
        double x = i * M_PI / 8;
        printf("sin(%.4f) = %+.6f  cos = %+.6f\n", x, sin(x), cos(x));
    }
    printf("sqrt(2) = %.15f\n", sqrt(2.0));
    return 0;
}
