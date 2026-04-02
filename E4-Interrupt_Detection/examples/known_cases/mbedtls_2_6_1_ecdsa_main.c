#include "mbedtls/bignum.h"
#include "mbedtls/ecdsa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  unsigned long state;
} FakeRngCtx;

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
  return (int) value;
}

static int use_random_input(void) {
  const char *env = getenv("AID_RANDOM_INPUT");
  return env != NULL && env[0] != '\0' && strcmp(env, "0") != 0;
}

static int fake_rng(void *p_rng, unsigned char *buf, size_t len) {
  FakeRngCtx *ctx = (FakeRngCtx *) p_rng;
  for (size_t i = 0; i < len; i++) {
    ctx->state = ctx->state * 1664525UL + 1013904223UL;
    buf[i] = (unsigned char) ((ctx->state >> 16) & 0xFFU);
  }
  return 0;
}

static int run_core(int randomize) {
  int rc = 1;
  mbedtls_ecp_group grp;
  mbedtls_mpi k;
  mbedtls_mpi inv;

  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&k);
  mbedtls_mpi_init(&inv);

  if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) != 0) {
    goto cleanup;
  }

  if (mbedtls_mpi_read_string(&k, 16,
                              randomize ? "2468ACE013579BDF2468ACE013579BDF"
                                        : "123456789ABCDEF0123456789ABCDEF0") != 0) {
    goto cleanup;
  }

  if (mbedtls_mpi_inv_mod(&inv, &k, &grp.N) != 0) {
    goto cleanup;
  }

  rc = (mbedtls_mpi_cmp_int(&inv, 0) != 0) ? 0 : 1;

cleanup:
  mbedtls_mpi_free(&inv);
  mbedtls_mpi_free(&k);
  mbedtls_ecp_group_free(&grp);
  return rc;
}

static int run_api(int randomize, int iter_index) {
  int rc = 1;
  mbedtls_ecdsa_context ctx;
  unsigned char hash[32];
  mbedtls_mpi r;
  mbedtls_mpi s;
  FakeRngCtx rng = {0xA5A5A5A5UL + (unsigned long) iter_index};

  memset(hash, randomize ? 0x5A : 0xA5, sizeof(hash));
  if (randomize) {
    hash[0] ^= (unsigned char) (iter_index & 0xFF);
    hash[1] ^= (unsigned char) ((iter_index >> 1) & 0xFF);
  }

  mbedtls_ecdsa_init(&ctx);
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&s);

  if (mbedtls_ecdsa_genkey(&ctx, MBEDTLS_ECP_DP_SECP256R1, fake_rng, &rng) != 0) {
    goto cleanup;
  }

  if (mbedtls_ecdsa_sign(&ctx.grp, &r, &s, &ctx.d,
                         hash, sizeof(hash), fake_rng, &rng) != 0) {
    goto cleanup;
  }

  if (mbedtls_ecdsa_verify(&ctx.grp, hash, sizeof(hash), &ctx.Q, &r, &s) != 0) {
    goto cleanup;
  }

  rc = 0;

cleanup:
  mbedtls_mpi_free(&s);
  mbedtls_mpi_free(&r);
  mbedtls_ecdsa_free(&ctx);
  return rc;
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
      if (run_api(randomize, i) != 0) {
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
      if (run_api(randomize, i) != 0) {
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
