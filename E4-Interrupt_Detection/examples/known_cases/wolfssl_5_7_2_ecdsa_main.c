#include "wolfssl/wolfcrypt/ecc.h"
#include "wolfssl/wolfcrypt/integer.h"
#include "wolfssl/wolfcrypt/random.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int get_iterations(void) {
  const char *env = getenv("AID_KNOWN_ITERS");
  if (env == NULL || env[0] == '\0') {
    return 100;
  }
  char *end = NULL;
  long value = strtol(env, &end, 10);
  if (end == env || value < 1) {
    return 100;
  }
  if (value > 1000000000L) {
    value = 1000000000L;
  }
  return (int)value;
}

static int use_random_input(void) {
  const char *env = getenv("AID_RANDOM_INPUT");
  return env != NULL && env[0] != '\0' && strcmp(env, "0") != 0;
}

static void invmod_probe(mp_int *a, mp_int *b, mp_int *c) {
  if (mp_invmod_slow(a, b, c) != MP_OKAY) {
    fprintf(stderr, "[wolfssl-ecc] mp_invmod_slow probe failed\n");
  }
}

static int run_core(int randomize) {
  mp_int a;
  mp_int b;
  mp_int c;
  int ret = 1;

  if (mp_init_multi(&a, &b, &c, NULL, NULL, NULL) != MP_OKAY) {
    return 1;
  }

  if (mp_read_radix(&a,
                    (char*)(randomize ?
                        "8c14b793cb19137e323a6d2e2a870bca2e7a493ec1153b3a95feb8a4873f8d09" :
                        "8c14b793cb19137e323a6d2e2a870bca2e7a493ec1153b3a95feb8a4873f8d08"),
                    16) != MP_OKAY ||
      mp_read_radix(&b,
                    (char*)"FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551",
                    16) != MP_OKAY) {
    goto cleanup;
  }

  invmod_probe(&a, &b, &c);
  ret = 0;

cleanup:
  mp_clear(&a);
  mp_clear(&b);
  mp_clear(&c);
  return ret;
}

static int run_api(int randomize) {
  static const char qx[] =
      "7a4e287890a1a47ad3457e52f2f76a83ce46cbc947616d0cbaa82323818a793d";
  static const char qy[] =
      "eec4084f5b29ebf29c44cce3b3059610922f8b30ea6e8811742ac7238fe87308";
  static const char d[] =
      "8c14b793cb19137e323a6d2e2a870bca2e7a493ec1153b3a95feb8a4873f8d08";
  static const char order[] =
      "FFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551";
  ecc_key key;
  WC_RNG rng;
  byte hash[32];
  byte sig[80];
  word32 sigLen = sizeof(sig);
  int verify = 0;
  mp_int a;
  mp_int b;
  mp_int c;
  int ret = 1;

  memset(&rng, 0, sizeof(rng));
  memset(hash, randomize ? 0x5A : 0xA5, sizeof(hash));

  if (mp_init_multi(&a, &b, &c, NULL, NULL, NULL) != MP_OKAY) {
    return 1;
  }

  wc_ecc_init(&key);
  if (wc_ecc_import_raw(&key, qx, qy, d, "SECP256R1") != 0) {
    goto cleanup;
  }

  if (wc_ecc_sign_hash(hash, sizeof(hash), sig, &sigLen, &rng, &key) != 0) {
    goto cleanup;
  }

  if (wc_ecc_verify_hash(sig, sigLen, hash, sizeof(hash), &verify, &key) != 0 || verify != 1) {
    goto cleanup;
  }

  if (mp_read_radix(&a, (char*)d, 16) != MP_OKAY ||
      mp_read_radix(&b, (char*)order, 16) != MP_OKAY) {
    goto cleanup;
  }
  invmod_probe(&a, &b, &c);
  ret = 0;

cleanup:
  wc_ecc_free(&key);
  mp_clear(&a);
  mp_clear(&b);
  mp_clear(&c);
  return ret;
}

int main(int argc, char **argv) {
  const char *mode = "both";
  int iterations = get_iterations();
  int randomize = use_random_input();
  if (argc > 1 && argv[1] != NULL) {
    mode = argv[1];
  }

  if (strcmp(mode, "api") == 0) {
    for (int i = 0; i < iterations; i++) {
      if (run_api(randomize) != 0) {
        return 1;
      }
    }
    return 0;
  }
  if (strcmp(mode, "core") == 0) {
    for (int i = 0; i < iterations; i++) {
      if (run_core(randomize) != 0) {
        return 1;
      }
    }
    return 0;
  }
  if (strcmp(mode, "both") == 0) {
    for (int i = 0; i < iterations; i++) {
      if (run_api(randomize) != 0) {
        return 1;
      }
    }
    for (int i = 0; i < iterations; i++) {
      if (run_core(randomize) != 0) {
        return 1;
      }
    }
    return 0;
  }

  fprintf(stderr, "Usage: %s [api|core|both]\n", argv[0]);
  return 2;
}
