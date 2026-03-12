#include <stdio.h>

void __intr_detect_hook(const char *func) {
  if (!func) {
    return;
  }
  fprintf(stderr, "[intr_detect] %s\n", func);
}
