#include "wolfssl/wolfcrypt/integer.h"
#include "wolfssl/wolfcrypt/rsa.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

static void invmod_probe(mp_int *a, mp_int *b, mp_int *c) {
  const char *repeat_env = getenv("AID_WOLFSSL_REPEAT");
  int repeat = 1;
  if (repeat_env && repeat_env[0] != '\0') {
    repeat = atoi(repeat_env);
  }
  if (repeat < 1) {
    repeat = 1;
  }
  for (int i = 0; i < repeat; i++) {
    if (mp_set_int(a, 17) != MP_OKAY || mp_set_int(b, 3233) != MP_OKAY) {
      return;
    }
    if (mp_invmod_slow(a, b, c) != MP_OKAY) {
      fprintf(stderr, "[wolfssl] mp_invmod_slow probe failed\n");
      return;
    }
  }
}

static int run_core(void) {
  mp_int a;
  mp_int b;
  mp_int c;
  mp_int e;
  mp_int n;
  mp_int m;
  mp_int rsa_out;

  if (mp_init_multi(&a, &b, &c, &e, &n, &m) != MP_OKAY) {
    return 1;
  }
  if (mp_init(&rsa_out) != MP_OKAY) {
    mp_clear(&a);
    mp_clear(&b);
    mp_clear(&c);
    mp_clear(&e);
    mp_clear(&n);
    mp_clear(&m);
    return 1;
  }

  /* 直接调用 bignum：rsa_out = m^e mod n */
  if (mp_set_int(&m, 65) != MP_OKAY ||
      mp_set_int(&e, 17) != MP_OKAY ||
      mp_set_int(&n, 3233) != MP_OKAY) {
    mp_clear(&a);
    mp_clear(&b);
    mp_clear(&c);
    mp_clear(&e);
    mp_clear(&n);
    mp_clear(&m);
    mp_clear(&rsa_out);
    return 1;
  }

  if (mp_exptmod(&m, &e, &n, &rsa_out) != MP_OKAY) {
    fprintf(stderr, "[wolfssl] mp_exptmod failed, fallback to m\n");
    mp_copy(&rsa_out, &m);
  }

  /* 使用结果触发 invmod_slow 的敏感分支 */
  if (mp_copy(&a, &rsa_out) != MP_OKAY || mp_copy(&b, &n) != MP_OKAY) {
    mp_clear(&a);
    mp_clear(&b);
    mp_clear(&c);
    mp_clear(&e);
    mp_clear(&n);
    mp_clear(&m);
    mp_clear(&rsa_out);
    return 1;
  }
  if (mp_invmod_slow(&a, &b, &c) != MP_OKAY) {
    fprintf(stderr, "[wolfssl] mp_invmod_slow failed, ignore\n");
    invmod_probe(&a, &b, &c);
  }

  mp_clear(&a);
  mp_clear(&b);
  mp_clear(&c);
  mp_clear(&e);
  mp_clear(&n);
  mp_clear(&m);
  mp_clear(&rsa_out);
  return 0;
}

static int run_api(void) {
  mp_int a;
  mp_int b;
  mp_int c;
  mp_int n;
  mp_int rsa_out;
  mp_int tmp;
  RsaKey rsa;

  if (mp_init_multi(&a, &b, &c, &n, &rsa_out, &tmp) != MP_OKAY) {
    return 1;
  }
  if (wc_InitRsaKey(&rsa, NULL) != 0) {
    mp_clear(&a);
    mp_clear(&b);
    mp_clear(&c);
    mp_clear(&n);
    mp_clear(&rsa_out);
    mp_clear(&tmp);
    return 1;
  }

  if (mp_set_int(&n, 3233) != MP_OKAY ||
      mp_set_int(&rsa.n, 3233) != MP_OKAY ||
      mp_set_int(&rsa.e, 17) != MP_OKAY ||
      mp_set_int(&rsa.d, 2753) != MP_OKAY ||
      mp_set_int(&rsa.p, 61) != MP_OKAY ||
      mp_set_int(&rsa.q, 53) != MP_OKAY ||
      mp_set_int(&rsa.dP, 53) != MP_OKAY ||
      mp_set_int(&rsa.dQ, 49) != MP_OKAY ||
      mp_set_int(&rsa.u, 38) != MP_OKAY) {
    wc_FreeRsaKey(&rsa);
    mp_clear(&a);
    mp_clear(&b);
    mp_clear(&c);
    mp_clear(&n);
    mp_clear(&rsa_out);
    mp_clear(&tmp);
    return 1;
  }
  rsa.type = RSA_PRIVATE;
  byte in_raw[2] = {0x00, 0x2A};
  byte mid_raw[2] = {0};
  byte out_raw[2] = {0};
  word32 mid_sz = sizeof(mid_raw);
  word32 out_sz = sizeof(out_raw);
  if (wc_RsaDirect(in_raw, sizeof(in_raw), mid_raw, &mid_sz, &rsa,
                   RSA_PUBLIC_ENCRYPT, NULL) < 0 ||
      wc_RsaDirect(mid_raw, mid_sz, out_raw, &out_sz, &rsa,
                   RSA_PRIVATE_DECRYPT, NULL) < 0 ||
      out_sz != sizeof(in_raw) ||
      memcmp(in_raw, out_raw, sizeof(in_raw)) != 0 ||
      mp_read_unsigned_bin(&rsa_out, mid_raw, mid_sz) != MP_OKAY) {
    return 1;
  }

  /* 使用 RSA 结果触发 invmod_slow 的敏感分支 */
  if (mp_copy(&a, &rsa_out) != MP_OKAY || mp_copy(&b, &n) != MP_OKAY) {
    wc_FreeRsaKey(&rsa);
    mp_clear(&a);
    mp_clear(&b);
    mp_clear(&c);
    mp_clear(&n);
    mp_clear(&rsa_out);
    mp_clear(&tmp);
    return 1;
  }
  if (mp_invmod_slow(&a, &b, &c) != MP_OKAY) {
    fprintf(stderr, "[wolfssl] mp_invmod_slow failed, ignore\n");
    invmod_probe(&a, &b, &c);
  }

  wc_FreeRsaKey(&rsa);
  mp_clear(&a);
  mp_clear(&b);
  mp_clear(&c);
  mp_clear(&n);
  mp_clear(&rsa_out);
  mp_clear(&tmp);
  return 0;
}

int main(int argc, char **argv) {
  const char *mode = "both";
  int iterations = get_iterations();
  if (argc > 1 && argv[1] != NULL) {
    mode = argv[1];
  }

  if (strcmp(mode, "api") == 0) {
    for (int i = 0; i < iterations; i++) {
      if (run_api() != 0) {
        return 1;
      }
    }
    return 0;
  }
  if (strcmp(mode, "core") == 0) {
    for (int i = 0; i < iterations; i++) {
      if (run_core() != 0) {
        return 1;
      }
    }
    return 0;
  }
  if (strcmp(mode, "both") == 0) {
    for (int i = 0; i < iterations; i++) {
      if (run_api() != 0) {
        return 1;
      }
    }
    for (int i = 0; i < iterations; i++) {
      if (run_core() != 0) {
        return 1;
      }
    }
    return 0;
  }

  fprintf(stderr, "Usage: %s [api|core|both]\n", argv[0]);
  return 2;
}
