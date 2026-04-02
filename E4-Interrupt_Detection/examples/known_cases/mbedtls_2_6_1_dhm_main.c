#include "mbedtls/bignum.h"
#include "mbedtls/dhm.h"
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
  return (int)value;
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
  mbedtls_mpi g;
  mbedtls_mpi x;
  mbedtls_mpi p;
  mbedtls_mpi y;
  mbedtls_mpi inv;

  mbedtls_mpi_init(&g);
  mbedtls_mpi_init(&x);
  mbedtls_mpi_init(&p);
  mbedtls_mpi_init(&y);
  mbedtls_mpi_init(&inv);

  if (mbedtls_mpi_read_string(&g, 10, "2") != 0 ||
      mbedtls_mpi_read_string(&x, 10, randomize ? "193" : "191") != 0 ||
      mbedtls_mpi_read_string(&p, 10, "7919") != 0) {
    goto cleanup;
  }

  if (mbedtls_mpi_exp_mod(&y, &g, &x, &p, NULL) != 0) {
    goto cleanup;
  }
  if (mbedtls_mpi_inv_mod(&inv, &y, &p) != 0) {
    goto cleanup;
  }

  rc = (mbedtls_mpi_cmp_int(&inv, 0) != 0) ? 0 : 1;

cleanup:
  mbedtls_mpi_free(&inv);
  mbedtls_mpi_free(&y);
  mbedtls_mpi_free(&p);
  mbedtls_mpi_free(&x);
  mbedtls_mpi_free(&g);
  return rc;
}

static int run_api(int randomize, int iter_index) {
  int rc = 1;
  mbedtls_dhm_context alice;
  mbedtls_dhm_context bob;
  unsigned char alice_pub[256];
  unsigned char bob_pub[256];
  unsigned char alice_secret[256];
  unsigned char bob_secret[256];
  size_t alice_secret_len = 0;
  size_t bob_secret_len = 0;
  FakeRngCtx alice_rng = {0xC0FFEE11UL + (unsigned long) iter_index};
  FakeRngCtx bob_rng = {0xFACE1234UL + (unsigned long) iter_index};

  mbedtls_dhm_init(&alice);
  mbedtls_dhm_init(&bob);

  if (mbedtls_mpi_read_string(&alice.P, 10, "7919") != 0 ||
      mbedtls_mpi_read_string(&alice.G, 10, "2") != 0 ||
      mbedtls_mpi_copy(&bob.P, &alice.P) != 0 ||
      mbedtls_mpi_copy(&bob.G, &alice.G) != 0) {
    goto cleanup;
  }

  alice.len = (size_t)mbedtls_mpi_size(&alice.P);
  bob.len = (size_t)mbedtls_mpi_size(&bob.P);

  if (randomize) {
    alice_rng.state ^= 0x55AA55AAUL;
    bob_rng.state ^= 0xAA55AA55UL;
  }

  if (mbedtls_dhm_make_public(&alice, 2, alice_pub, alice.len, fake_rng, &alice_rng) != 0 ||
      mbedtls_dhm_read_public(&bob, alice_pub, alice.len) != 0 ||
      mbedtls_dhm_make_public(&bob, 2, bob_pub, bob.len, fake_rng, &bob_rng) != 0 ||
      mbedtls_dhm_read_public(&alice, bob_pub, bob.len) != 0) {
    goto cleanup;
  }

  if (mbedtls_dhm_calc_secret(&alice, alice_secret, sizeof(alice_secret), &alice_secret_len, fake_rng, &alice_rng) != 0 ||
      mbedtls_dhm_calc_secret(&bob, bob_secret, sizeof(bob_secret), &bob_secret_len, fake_rng, &bob_rng) != 0) {
    goto cleanup;
  }

  if (alice_secret_len != bob_secret_len ||
      memcmp(alice_secret, bob_secret, alice_secret_len) != 0) {
    goto cleanup;
  }

  rc = 0;

cleanup:
  mbedtls_dhm_free(&bob);
  mbedtls_dhm_free(&alice);
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
