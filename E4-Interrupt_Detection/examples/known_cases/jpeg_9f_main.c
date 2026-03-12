#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JPEG_INTERNALS
#include "jpeglib.h"
#include "jdct.h"

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

static unsigned char *read_file(const char *path, size_t *out_size) {
  FILE *fp = fopen(path, "rb");
  if (fp == NULL) {
    return NULL;
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return NULL;
  }
  long len = ftell(fp);
  if (len <= 0) {
    fclose(fp);
    return NULL;
  }
  if (fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return NULL;
  }
  unsigned char *buf = (unsigned char *)malloc((size_t)len);
  if (buf == NULL) {
    fclose(fp);
    return NULL;
  }
  size_t nread = fread(buf, 1, (size_t)len, fp);
  fclose(fp);
  if (nread != (size_t)len) {
    free(buf);
    return NULL;
  }
  *out_size = (size_t)len;
  return buf;
}

static JSAMPLE *alloc_range_limit(void) {
  size_t count = (size_t)(RANGE_CENTER * 2 + MAXJSAMPLE + 1);
  JSAMPLE *base = (JSAMPLE *)malloc(count * sizeof(JSAMPLE));
  if (base == NULL) {
    return NULL;
  }
  memset(base, 0, RANGE_CENTER * sizeof(JSAMPLE));
  JSAMPLE *table = base + RANGE_CENTER;
  int i = 0;
  for (i = 0; i <= MAXJSAMPLE; i++) {
    table[i] = (JSAMPLE)i;
  }
  for (; i <= MAXJSAMPLE + RANGE_CENTER; i++) {
    table[i] = MAXJSAMPLE;
  }
  return table;
}

static int run_core(void) {
  struct jpeg_decompress_struct cinfo;
  jpeg_component_info comp;
  ISLOW_MULT_TYPE qtbl[DCTSIZE2];
  JCOEF coef_block[DCTSIZE2];
  JSAMPLE output[DCTSIZE][DCTSIZE];
  JSAMPROW rows[DCTSIZE];
  int i = 0;

  memset(&cinfo, 0, sizeof(cinfo));
  memset(&comp, 0, sizeof(comp));
  memset(qtbl, 0, sizeof(qtbl));
  memset(coef_block, 0, sizeof(coef_block));
  memset(output, 0, sizeof(output));

  for (i = 0; i < DCTSIZE2; i++) {
    qtbl[i] = 1;
  }
  coef_block[0] = 1;

  for (i = 0; i < DCTSIZE; i++) {
    rows[i] = output[i];
  }

  cinfo.sample_range_limit = alloc_range_limit();
  if (cinfo.sample_range_limit == NULL) {
    return 1;
  }

  comp.dct_table = qtbl;

  jpeg_idct_islow(&cinfo, &comp, coef_block, rows, 0);

  free(cinfo.sample_range_limit - RANGE_CENTER);
  return 0;
}

static int run_api(void) {
  const char *path = getenv("INTMON_JPEG_FILE");
  if (path == NULL || path[0] == '\0') {
    path = "third_party/jpeg-9f/testimg.jpg";
  }

  size_t data_size = 0;
  unsigned char *data = read_file(path, &data_size);
  if (data == NULL) {
    fprintf(stderr, "[intmon] 读取 JPEG 失败: %s\n", path);
    return 1;
  }

  struct jpeg_decompress_struct cinfo;
  struct jpeg_error_mgr jerr;
  memset(&cinfo, 0, sizeof(cinfo));
  cinfo.err = jpeg_std_error(&jerr);

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, data, data_size);
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    free(data);
    return 1;
  }
  cinfo.dct_method = JDCT_ISLOW;

  if (!jpeg_start_decompress(&cinfo)) {
    jpeg_destroy_decompress(&cinfo);
    free(data);
    return 1;
  }

  size_t row_stride = (size_t)cinfo.output_width *
                      (size_t)cinfo.output_components;
  JSAMPLE *rowbuf = (JSAMPLE *)malloc(row_stride);
  if (rowbuf == NULL) {
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    free(data);
    return 1;
  }

  while (cinfo.output_scanline < cinfo.output_height) {
    JSAMPROW row = rowbuf;
    jpeg_read_scanlines(&cinfo, &row, 1);
  }

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  free(rowbuf);
  free(data);
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
