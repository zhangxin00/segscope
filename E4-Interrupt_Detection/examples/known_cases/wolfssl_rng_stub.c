#include "wolfssl/wolfcrypt/types.h"

typedef struct WC_RNG WC_RNG;

int wc_RNG_GenerateBlock(WC_RNG* rng, byte* output, word32 sz) {
  (void)rng;
  if (output == 0) {
    return -1;
  }
  for (word32 i = 0; i < sz; i++) {
    output[i] = (byte)(i & 0xFF);
  }
  return 0;
}
