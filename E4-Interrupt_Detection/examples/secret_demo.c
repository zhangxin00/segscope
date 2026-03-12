#include <stdio.h>

static int foo(int secret) {
  if (secret > 0) {
    return 1;
  }
  return 0;
}

int main(void) {
  int secret = 1;
  return foo(secret);
}
