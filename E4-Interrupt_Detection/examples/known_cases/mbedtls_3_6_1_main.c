#include "mbedtls/bignum.h"
#include "mbedtls/rsa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int get_iterations(void) {
  const char *env = getenv("AID_KNOWN_ITERS");
  if (env == NULL || env[0] == '\0') {
    return 10000;
  }
  char *end = NULL;
  long value = strtol(env, &end, 10);
  if (end == env || value < 1) {
    return 10000;
  }
  if (value > 1000000000L) {
    value = 1000000000L;
  }
  return (int)value;
}

static int run_core(void) {
  mbedtls_mpi A;
  mbedtls_mpi B;
  mbedtls_mpi G;
  mbedtls_mpi X;
  mbedtls_mpi E;
  mbedtls_mpi N;
  mbedtls_mpi C;

  mbedtls_mpi_init(&A);
  mbedtls_mpi_init(&B);
  mbedtls_mpi_init(&G);
  mbedtls_mpi_init(&X);
  mbedtls_mpi_init(&E);
  mbedtls_mpi_init(&N);
  mbedtls_mpi_init(&C);

  /* 直接调用 bignum：exp_mod 触发内部敏感分支 */
  mbedtls_mpi_lset(&A, 42);
  mbedtls_mpi_lset(&E, 17);
  mbedtls_mpi_lset(&N, 3233);
  if (mbedtls_mpi_exp_mod(&C, &A, &E, &N, NULL) != 0) {
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&G);
    mbedtls_mpi_free(&X);
    mbedtls_mpi_free(&E);
    mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&C);
    return 1;
  }

  /* 触发 gcd / inv_mod 的敏感分支 */
  if (mbedtls_mpi_copy(&B, &N) != 0) {
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&G);
    mbedtls_mpi_free(&X);
    mbedtls_mpi_free(&E);
    mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&C);
    return 1;
  }
  if (mbedtls_mpi_gcd(&G, &C, &B) != 0) {
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&G);
    mbedtls_mpi_free(&X);
    mbedtls_mpi_free(&E);
    mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&C);
    return 1;
  }
  if (mbedtls_mpi_inv_mod(&X, &C, &B) != 0) {
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&G);
    mbedtls_mpi_free(&X);
    mbedtls_mpi_free(&E);
    mbedtls_mpi_free(&N);
    mbedtls_mpi_free(&C);
    return 1;
  }

  mbedtls_mpi_free(&A);
  mbedtls_mpi_free(&B);
  mbedtls_mpi_free(&G);
  mbedtls_mpi_free(&X);
  mbedtls_mpi_free(&E);
  mbedtls_mpi_free(&N);
  mbedtls_mpi_free(&C);

  return 0;
}

static int run_api(void) {
  mbedtls_mpi A;
  mbedtls_mpi B;
  mbedtls_mpi G;
  mbedtls_mpi X;
  mbedtls_mpi Nmpi;
  mbedtls_mpi Empi;
  mbedtls_rsa_context rsa;

  mbedtls_mpi_init(&A);
  mbedtls_mpi_init(&B);
  mbedtls_mpi_init(&G);
  mbedtls_mpi_init(&X);
  mbedtls_mpi_init(&Nmpi);
  mbedtls_mpi_init(&Empi);
  mbedtls_rsa_init(&rsa);

  /* RSA 原始 API：public 操作 */
  if (mbedtls_mpi_lset(&Nmpi, 3233) != 0 ||
      mbedtls_mpi_lset(&Empi, 17) != 0) {
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&G);
    mbedtls_mpi_free(&X);
    mbedtls_mpi_free(&Nmpi);
    mbedtls_mpi_free(&Empi);
    mbedtls_rsa_free(&rsa);
    return 1;
  }
  if (mbedtls_rsa_import(&rsa, &Nmpi, NULL, NULL, NULL, &Empi) != 0) {
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&G);
    mbedtls_mpi_free(&X);
    mbedtls_mpi_free(&Nmpi);
    mbedtls_mpi_free(&Empi);
    mbedtls_rsa_free(&rsa);
    return 1;
  }
  if (mbedtls_rsa_complete(&rsa) != 0) {
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&G);
    mbedtls_mpi_free(&X);
    mbedtls_mpi_free(&Nmpi);
    mbedtls_mpi_free(&Empi);
    mbedtls_rsa_free(&rsa);
    return 1;
  }
  size_t rsa_len = mbedtls_rsa_get_len(&rsa);
  if (rsa_len == 0 || rsa_len > 8) {
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&G);
    mbedtls_mpi_free(&X);
    mbedtls_mpi_free(&Nmpi);
    mbedtls_mpi_free(&Empi);
    mbedtls_rsa_free(&rsa);
    return 1;
  }
  unsigned char rsa_in[8] = {0};
  unsigned char rsa_out[8] = {0};
  rsa_in[rsa_len - 1] = 0x2A;
  if (mbedtls_rsa_public(&rsa, rsa_in, rsa_out) != 0) {
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&G);
    mbedtls_mpi_free(&X);
    mbedtls_mpi_free(&Nmpi);
    mbedtls_mpi_free(&Empi);
    mbedtls_rsa_free(&rsa);
    return 1;
  }

  /* 使用 RSA 结果触发 gcd / inv_mod 的敏感分支 */
  if (mbedtls_mpi_read_binary(&A, rsa_out, rsa_len) != 0 ||
      mbedtls_mpi_copy(&B, &Nmpi) != 0) {
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&G);
    mbedtls_mpi_free(&X);
    mbedtls_mpi_free(&Nmpi);
    mbedtls_mpi_free(&Empi);
    mbedtls_rsa_free(&rsa);
    return 1;
  }
  if (mbedtls_mpi_gcd(&G, &A, &B) != 0) {
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&G);
    mbedtls_mpi_free(&X);
    mbedtls_mpi_free(&Nmpi);
    mbedtls_mpi_free(&Empi);
    mbedtls_rsa_free(&rsa);
    return 1;
  }
  if (mbedtls_mpi_inv_mod(&X, &A, &B) != 0) {
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&G);
    mbedtls_mpi_free(&X);
    mbedtls_mpi_free(&Nmpi);
    mbedtls_mpi_free(&Empi);
    mbedtls_rsa_free(&rsa);
    return 1;
  }

  mbedtls_mpi_free(&A);
  mbedtls_mpi_free(&B);
  mbedtls_mpi_free(&G);
  mbedtls_mpi_free(&X);
  mbedtls_mpi_free(&Nmpi);
  mbedtls_mpi_free(&Empi);
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
