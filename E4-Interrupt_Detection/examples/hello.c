#include <stdio.h>

static int add(int a, int b) {
  return a + b;
}

int main(void) {
  int v = add(1, 2);
  if (v > 0) {
    printf("v=%d\n", v);
  }
  return v == 3 ? 0 : 1;
}
