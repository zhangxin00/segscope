#include "mbedtls/bignum.h"
#include "mbedtls/rsa.h"
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

static int run_core(void) {
  mbedtls_mpi A;
  mbedtls_mpi B;
  mbedtls_mpi E;
  mbedtls_mpi N;
  mbedtls_mpi X;
  mbedtls_mpi C;

  mbedtls_mpi_init(&A);
  mbedtls_mpi_init(&B);
  mbedtls_mpi_init(&E);
  mbedtls_mpi_init(&N);
  mbedtls_mpi_init(&X);
  mbedtls_mpi_init(&C);

  /* 直接调用 bignum：exp_mod 触发 montmul 分支 */
  mbedtls_mpi_lset(&A, 65);
  mbedtls_mpi_lset(&E, 17);
  mbedtls_mpi_lset(&N, 3233);
  if (mbedtls_mpi_exp_mod(&C, &A, &E, &N, NULL) != 0) {
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&E);
    mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&X);
    mbedtls_mpi_free(&C);
    return 1;
  }

  /* 触发 add/sub 的符号分支 */
  if (mbedtls_mpi_copy(&B, &C) != 0) {
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&E);
    mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&X);
    mbedtls_mpi_free(&C);
    return 1;
  }
  B.s = -1;
  if (mbedtls_mpi_add_mpi(&X, &A, &B) != 0) {
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&E);
    mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&X);
    mbedtls_mpi_free(&C);
    return 1;
  }
  if (mbedtls_mpi_sub_mpi(&X, &A, &B) != 0) {
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&E);
    mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&X);
    mbedtls_mpi_free(&C);
    return 1;
  }

  mbedtls_mpi_free(&A);
  mbedtls_mpi_free(&B);
  mbedtls_mpi_free(&E);
  mbedtls_mpi_free(&N);
  mbedtls_mpi_free(&X);
  mbedtls_mpi_free(&C);
  return 0;
}

static int run_api(void) {
  mbedtls_mpi A;
  mbedtls_mpi B;
  mbedtls_mpi D;
  mbedtls_mpi P;
  mbedtls_mpi Q;
  mbedtls_mpi DP;
  mbedtls_mpi DQ;
  mbedtls_mpi QP;
  mbedtls_mpi X;
  mbedtls_rsa_context rsa;

  mbedtls_mpi_init(&A);
  mbedtls_mpi_init(&B);
  mbedtls_mpi_init(&D);
  mbedtls_mpi_init(&P);
  mbedtls_mpi_init(&Q);
  mbedtls_mpi_init(&DP);
  mbedtls_mpi_init(&DQ);
  mbedtls_mpi_init(&QP);
  mbedtls_mpi_init(&X);
  mbedtls_rsa_init(&rsa, MBEDTLS_RSA_PKCS_V15, 0);

  if (mbedtls_mpi_lset(&rsa.N, 3233) != 0 ||
      mbedtls_mpi_lset(&rsa.E, 17) != 0 ||
      mbedtls_mpi_lset(&D, 2753) != 0 ||
      mbedtls_mpi_lset(&P, 61) != 0 ||
      mbedtls_mpi_lset(&Q, 53) != 0 ||
      mbedtls_mpi_lset(&DP, 53) != 0 ||
      mbedtls_mpi_lset(&DQ, 49) != 0 ||
      mbedtls_mpi_lset(&QP, 38) != 0 ||
      mbedtls_mpi_copy(&rsa.D, &D) != 0 ||
      mbedtls_mpi_copy(&rsa.P, &P) != 0 ||
      mbedtls_mpi_copy(&rsa.Q, &Q) != 0 ||
      mbedtls_mpi_copy(&rsa.DP, &DP) != 0 ||
      mbedtls_mpi_copy(&rsa.DQ, &DQ) != 0 ||
      mbedtls_mpi_copy(&rsa.QP, &QP) != 0) {
      mbedtls_mpi_free(&A);
      mbedtls_mpi_free(&B);
      mbedtls_mpi_free(&D);
      mbedtls_mpi_free(&P);
      mbedtls_mpi_free(&Q);
      mbedtls_mpi_free(&DP);
      mbedtls_mpi_free(&DQ);
      mbedtls_mpi_free(&QP);
      mbedtls_mpi_free(&X);
      mbedtls_rsa_free(&rsa);
      return 1;
  }
  rsa.len = mbedtls_mpi_size(&rsa.N);
  if (rsa.len == 0 || rsa.len > 8) {
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&X);
    mbedtls_rsa_free(&rsa);
    return 1;
  }
  unsigned char rsa_in[8] = {0};
  unsigned char rsa_mid[8] = {0};
  unsigned char rsa_out[8] = {0};
  rsa_in[rsa.len - 1] = 0x2A;
  if (mbedtls_rsa_public(&rsa, rsa_in, rsa_mid) != 0 ||
      mbedtls_rsa_private(&rsa, NULL, NULL, rsa_mid, rsa_out) != 0 ||
      memcmp(rsa_in, rsa_out, rsa.len) != 0) {
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&D);
    mbedtls_mpi_free(&P);
    mbedtls_mpi_free(&Q);
    mbedtls_mpi_free(&DP);
    mbedtls_mpi_free(&DQ);
    mbedtls_mpi_free(&QP);
    mbedtls_mpi_free(&X);
    mbedtls_rsa_free(&rsa);
    return 1;
  }

  if (mbedtls_mpi_lset(&A, 65) != 0 ||
      mbedtls_mpi_read_binary(&B, rsa_mid, rsa.len) != 0) {
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&D);
    mbedtls_mpi_free(&P);
    mbedtls_mpi_free(&Q);
    mbedtls_mpi_free(&DP);
    mbedtls_mpi_free(&DQ);
    mbedtls_mpi_free(&QP);
    mbedtls_mpi_free(&X);
    mbedtls_rsa_free(&rsa);
    return 1;
  }
  B.s = -1;
  if (mbedtls_mpi_add_mpi(&X, &A, &B) != 0) {
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&X);
    mbedtls_rsa_free(&rsa);
    return 1;
  }
  if (mbedtls_mpi_sub_mpi(&X, &A, &B) != 0) {
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&X);
    mbedtls_rsa_free(&rsa);
    return 1;
  }

  mbedtls_mpi_free(&A);
  mbedtls_mpi_free(&B);
  mbedtls_mpi_free(&D);
  mbedtls_mpi_free(&P);
  mbedtls_mpi_free(&Q);
  mbedtls_mpi_free(&DP);
  mbedtls_mpi_free(&DQ);
  mbedtls_mpi_free(&QP);
  mbedtls_mpi_free(&X);
  mbedtls_rsa_free(&rsa);

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
