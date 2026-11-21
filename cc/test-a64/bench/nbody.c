// 浮動小数点と構造体（n 体問題の簡単な版）
#include <math.h>
#include <stdio.h>
typedef struct { double x, y, z, vx, vy, vz, m; } Body;
int main(void) {
  Body b[5] = {{0, 0, 0, 0, 0, 0, 39.47}, {4.84, -1.16, -0.10, 0.60, 2.81, -0.02, 0.037},
               {8.34, 4.12, -0.40, -1.01, 1.82, 0.008, 0.011}, {12.89, -15.11, -0.22, 1.08, 0.86, -0.01, 0.0017},
               {15.37, -25.91, 0.17, 0.97, 0.59, -0.03, 0.002}};
  for (int step = 0; step < 3000000; step++) {
    for (int i = 0; i < 5; i++)
      for (int j = i + 1; j < 5; j++) {
        double dx = b[i].x - b[j].x, dy = b[i].y - b[j].y, dz = b[i].z - b[j].z;
        double d2 = dx * dx + dy * dy + dz * dz, mag = 0.01 / (d2 * sqrt(d2));
        b[i].vx -= dx * b[j].m * mag; b[i].vy -= dy * b[j].m * mag; b[i].vz -= dz * b[j].m * mag;
        b[j].vx += dx * b[i].m * mag; b[j].vy += dy * b[i].m * mag; b[j].vz += dz * b[i].m * mag;
      }
    for (int i = 0; i < 5; i++) { b[i].x += 0.01 * b[i].vx; b[i].y += 0.01 * b[i].vy; b[i].z += 0.01 * b[i].vz; }
  }
  printf("%.9f %.9f\n", b[0].x, b[1].y);
  return 0;
}
