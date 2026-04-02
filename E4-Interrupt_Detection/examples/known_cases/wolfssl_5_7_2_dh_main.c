#include "wolfssl/wolfcrypt/dh.h"
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
    fprintf(stderr, "[wolfssl-dh] mp_invmod_slow probe failed\n");
  }
}

static int init_mpi_dec(mp_int* value, const char* dec) {
  return mp_read_radix(value, (char*)dec, 10);
}

static int run_core(int randomize) {
  mp_int g;
  mp_int p;
  mp_int x;
  mp_int y;
  mp_int z;
  mp_int inv;
  int ret = 1;

  if (mp_init_multi(&g, &p, &x, &y, &z, &inv) != MP_OKAY) {
    return 1;
  }

  if (init_mpi_dec(&g, "2") != MP_OKAY ||
      init_mpi_dec(&p, "7919") != MP_OKAY ||
      init_mpi_dec(&x, randomize ? "193" : "191") != MP_OKAY ||
      init_mpi_dec(&y, randomize ? "197" : "193") != MP_OKAY) {
    goto cleanup;
  }

  if (mp_exptmod(&g, &x, &p, &z) != MP_OKAY) {
    goto cleanup;
  }

  if (mp_copy(&inv, &z) != MP_OKAY) {
    goto cleanup;
  }
  invmod_probe(&inv, &p, &y);
  ret = 0;

cleanup:
  mp_clear(&g);
  mp_clear(&p);
  mp_clear(&x);
  mp_clear(&y);
  mp_clear(&z);
  mp_clear(&inv);
  return ret;
}

static int run_api(int randomize) {
  static const byte p_bytes[] = {0x1E, 0xEF};
  static const byte g_bytes[] = {0x02};
  DhKey alice;
  DhKey bob;
  WC_RNG rng;
  byte alice_priv[8];
  byte alice_pub[8];
  byte bob_priv[8];
  byte bob_pub[8];
  byte alice_agree[8];
  byte bob_agree[8];
  word32 alice_priv_sz = sizeof(alice_priv);
  word32 alice_pub_sz = sizeof(alice_pub);
  word32 bob_priv_sz = sizeof(bob_priv);
  word32 bob_pub_sz = sizeof(bob_pub);
  word32 alice_agree_sz = sizeof(alice_agree);
  word32 bob_agree_sz = sizeof(bob_agree);
  mp_int a;
  mp_int b;
  mp_int c;
  int ret = 1;

  memset(&rng, 0, sizeof(rng));
  if (mp_init_multi(&a, &b, &c, NULL, NULL, NULL) != MP_OKAY) {
    return 1;
  }
  if (wc_InitDhKey(&alice) != 0 || wc_InitDhKey(&bob) != 0) {
    goto cleanup_init;
  }
  if (wc_DhSetKey(&alice, p_bytes, sizeof(p_bytes), g_bytes, sizeof(g_bytes)) != 0 ||
      wc_DhSetKey(&bob, p_bytes, sizeof(p_bytes), g_bytes, sizeof(g_bytes)) != 0) {
    goto cleanup;
  }

  if (wc_DhGenerateKeyPair(&alice, &rng, alice_priv, &alice_priv_sz, alice_pub, &alice_pub_sz) != 0 ||
      wc_DhGenerateKeyPair(&bob, &rng, bob_priv, &bob_priv_sz, bob_pub, &bob_pub_sz) != 0) {
    goto cleanup;
  }

  if (wc_DhAgree(&alice, alice_agree, &alice_agree_sz, alice_priv, alice_priv_sz, bob_pub, bob_pub_sz) != 0 ||
      wc_DhAgree(&bob, bob_agree, &bob_agree_sz, bob_priv, bob_priv_sz, alice_pub, alice_pub_sz) != 0) {
    goto cleanup;
  }

  if (mp_read_unsigned_bin(&a, alice_agree, alice_agree_sz) != MP_OKAY ||
      init_mpi_dec(&b, "7919") != MP_OKAY) {
    goto cleanup;
  }
  invmod_probe(&a, &b, &c);
  ret = 0;

cleanup:
  wc_FreeDhKey(&bob);
  wc_FreeDhKey(&alice);
cleanup_init:
  (void)randomize;
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
