#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#if defined(INTMON_TIME_TSC) && (defined(__x86_64__) || defined(__i386__))
#define INTMON_TIME_SRC "tsc"
#define INTMON_TIME_UNIT "cycles"
#else
#define INTMON_TIME_SRC "monotonic"
#define INTMON_TIME_UNIT "ns"
#endif

static _Thread_local uint16_t tls_value;
static _Thread_local int func_depth;
static _Thread_local uint64_t func_enter_ticks;

static _Atomic uint64_t func_total_ticks;
_Atomic uint64_t intmon_interrupt_detect_count;
static uint64_t program_start_ticks;
static bool program_start_inited;

typedef struct {
  int64_t branch_id;
  int32_t path;
  uint64_t count;
  bool used;
} IntmonCountEntry;

static IntmonCountEntry *count_table;
static size_t count_cap;
static size_t count_size;

static uint64_t intmon_hash_u64(uint64_t v) {
  v ^= v >> 33;
  v *= 0xff51afd7ed558ccdULL;
  v ^= v >> 33;
  v *= 0xc4ceb9fe1a85ec53ULL;
  v ^= v >> 33;
  return v;
}

static uint64_t intmon_hash_key(int64_t branch_id, int32_t path) {
  uint64_t a = (uint64_t)branch_id;
  uint64_t b = (uint64_t)(uint32_t)path;
  return intmon_hash_u64(a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2)));
}

static void intmon_count_grow(void) {
  size_t new_cap = count_cap ? count_cap * 2 : 256;
  IntmonCountEntry *new_table =
      (IntmonCountEntry *)calloc(new_cap, sizeof(IntmonCountEntry));
  if (new_table == NULL) {
    return;
  }
  if (count_table) {
    for (size_t i = 0; i < count_cap; i++) {
      if (!count_table[i].used) {
        continue;
      }
      IntmonCountEntry entry = count_table[i];
      size_t mask = new_cap - 1;
      size_t idx =
          (size_t)(intmon_hash_key(entry.branch_id, entry.path) & mask);
      while (new_table[idx].used) {
        idx = (idx + 1) & mask;
      }
      new_table[idx] = entry;
      new_table[idx].used = true;
    }
    free(count_table);
  }
  count_table = new_table;
  count_cap = new_cap;
}

static void intmon_count_add(int64_t branch_id, int32_t path) {
  if (count_cap == 0 || count_size * 10 >= count_cap * 7) {
    intmon_count_grow();
  }
  if (count_cap == 0) {
    return;
  }
  size_t mask = count_cap - 1;
  size_t idx = (size_t)(intmon_hash_key(branch_id, path) & mask);
  while (count_table[idx].used) {
    if (count_table[idx].branch_id == branch_id &&
        count_table[idx].path == path) {
      count_table[idx].count++;
      return;
    }
    idx = (idx + 1) & mask;
  }
  count_table[idx].used = true;
  count_table[idx].branch_id = branch_id;
  count_table[idx].path = path;
  count_table[idx].count = 1;
  count_size++;
}

static void intmon_count_report(void) {
  if (count_table == NULL || count_size == 0) {
    return;
  }
  for (size_t i = 0; i < count_cap; i++) {
    if (!count_table[i].used) {
      continue;
    }
    if (count_table[i].path == INT32_MIN) {
      fprintf(stderr,
              "[intmon] COUNT prepare id=%" PRId64 " total=%" PRIu64 "\n",
              count_table[i].branch_id, count_table[i].count);
    } else {
      fprintf(stderr,
              "[intmon] COUNT check id=%" PRId64 " path=%d total=%" PRIu64 "\n",
              count_table[i].branch_id, count_table[i].path,
              count_table[i].count);
    }
  }
}

#if defined(INTMON_TIME_TSC) && (defined(__x86_64__) || defined(__i386__))
static inline uint64_t intmon_read_tsc(void) {
  uint32_t lo = 0;
  uint32_t hi = 0;
  __asm__ volatile("lfence\nrdtsc\nlfence"
                   : "=a"(lo), "=d"(hi)
                   :
                   : "memory");
  return ((uint64_t)hi << 32) | lo;
}
#endif

static uint64_t intmon_now_ticks(void) {
#if defined(INTMON_TIME_TSC) && (defined(__x86_64__) || defined(__i386__))
  return intmon_read_tsc();
#else
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static void intmon_report(void) {
  uint64_t end_ticks = intmon_now_ticks();
  if (!program_start_inited || end_ticks == 0) {
    return;
  }
  uint64_t total_ticks = end_ticks - program_start_ticks;
  uint64_t func_ticks = atomic_load(&func_total_ticks);
  if (func_depth > 0 && func_enter_ticks != 0) {
    func_ticks += (end_ticks - func_enter_ticks);
  }
  double func_pct = 0.0;
  if (total_ticks > 0) {
    func_pct = (double)func_ticks * 100.0 / (double)total_ticks;
  }
#if defined(INTMON_TIME_TSC) && (defined(__x86_64__) || defined(__i386__))
  fprintf(stderr,
          "[intmon] time source=%s unit=%s total_ticks=%" PRIu64
          " func_ticks=%" PRIu64 " func_pct=%.2f\n",
          INTMON_TIME_SRC, INTMON_TIME_UNIT, total_ticks, func_ticks, func_pct);
#else
  fprintf(stderr,
          "[intmon] time source=%s unit=%s total_ns=%" PRIu64
          " func_ns=%" PRIu64 " func_pct=%.2f\n",
          INTMON_TIME_SRC, INTMON_TIME_UNIT, total_ticks, func_ticks, func_pct);
#endif
}

static void intmon_interrupt_report(void) {
  uint64_t count = atomic_load(&intmon_interrupt_detect_count);
  fprintf(stderr, "[intmon] interrupt_detect total=%" PRIu64 "\n", count);
}

__attribute__((constructor)) static void intmon_init(void) {
  program_start_ticks = intmon_now_ticks();
  program_start_inited = (program_start_ticks != 0);
  atexit(intmon_report);
  atexit(intmon_count_report);
  atexit(intmon_interrupt_report);
}

void __intmon_prepare(int64_t branch_id) {
  (void)branch_id;
#if defined(INTMON_USE_GS)
  uint16_t value = 1;
  __asm__ volatile("movw %0, %%gs" : : "rm"(value) : "memory");
#else
  tls_value = 1;
#endif
}

void __intmon_check(int64_t branch_id, int32_t path) {
  (void)branch_id;
  (void)path;
  uint16_t value = 0;
#if defined(INTMON_USE_GS)
  __asm__ volatile("movw %%gs, %0" : "=rm"(value) : : "memory");
#else
  value = tls_value;
#endif
  if (value == 0) {
    atomic_fetch_add(&intmon_interrupt_detect_count, 1);
  }
}

void __intmon_prepare_cnt(int64_t branch_id) {
  intmon_count_add(branch_id, INT32_MIN);
}

void __intmon_check_cnt(int64_t branch_id, int32_t path) {
  intmon_count_add(branch_id, path);
}

void __intmon_div(int64_t branch_id, int32_t kind) {
  printf("[intmon] DIV id=%" PRId64 " kind=%d\n", branch_id, kind);
}

void __intmon_func_enter(int64_t func_id) {
  (void)func_id;
  if (func_depth == 0) {
    func_enter_ticks = intmon_now_ticks();
  }
  func_depth++;
}

void __intmon_func_exit(int64_t func_id) {
  (void)func_id;
  if (func_depth <= 0) {
    func_depth = 0;
    return;
  }
  func_depth--;
  if (func_depth == 0 && func_enter_ticks != 0) {
    uint64_t end_ticks = intmon_now_ticks();
    if (end_ticks != 0) {
      atomic_fetch_add(&func_total_ticks, end_ticks - func_enter_ticks);
    }
    func_enter_ticks = 0;
  }
}
