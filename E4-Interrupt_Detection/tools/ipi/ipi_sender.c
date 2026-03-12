#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

#define IPI_IOC_MAGIC 'i'
#define IPI_IOC_SEND _IOW(IPI_IOC_MAGIC, 1, struct ipi_request)
#define IPI_IOC_START_FLOOD _IOW(IPI_IOC_MAGIC, 2, struct ipi_flood_request)
#define IPI_IOC_STOP_FLOOD _IOR(IPI_IOC_MAGIC, 3, struct ipi_count)
#define IPI_IOC_START_TIMER _IOW(IPI_IOC_MAGIC, 4, struct ipi_timer_request)
#define IPI_IOC_STOP_TIMER _IOR(IPI_IOC_MAGIC, 5, struct ipi_count)
#define IPI_CPU_ANY 0xFFFFFFFFu
#define IPI_MODE_SMP 0u
#define IPI_MODE_APIC 1u

struct ipi_request {
  uint32_t cpu;
  uint32_t wait;
  uint32_t mode;
};

struct ipi_flood_request {
  uint32_t cpu;
  uint32_t sender;
  uint32_t wait;
  uint32_t rate;
  uint32_t mode;
};

struct ipi_timer_request {
  uint32_t cpu;
  uint32_t rate;
};

struct ipi_count {
  uint64_t sent;
  uint64_t handled;
};

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int sig) {
  (void)sig;
  g_stop = 1;
}

static void usage(const char *prog) {
  fprintf(stderr,
          "用法: %s -t <cpu> [-s <cpu>] [-r <rate>] [-D <sec>] [-d <dev>] [--wait] [--mode user|kthread|timer|apic]\n"
          "  -t, --target     目标 CPU（必填）\n"
          "  -s, --sender     发送端绑定 CPU（可选）\n"
          "  -r, --rate       发送频率（次/秒，<=0 表示尽可能快）\n"
          "  -D, --duration   发送时长（秒，<=0 表示直到收到信号）\n"
          "  -d, --device     设备路径（默认 /dev/ipi_ctl）\n"
          "  -w, --wait       等待 IPI 回调执行完毕（默认异步）\n"
          "  -m, --mode       发送模式：user(用户态) | kthread(内核线程) | timer(本地定时器) | apic(直写 APIC)\n"
          "  -q, --quiet      安静模式，仅输出错误\n",
          prog);
}

static int pin_to_cpu(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return sched_setaffinity(0, sizeof(set), &set);
}

static uint64_t now_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv) {
  const char *device = "/dev/ipi_ctl";
  int target_cpu = -1;
  int sender_cpu = -1;
  double rate = 0.0;
  double duration = 1.0;
  bool wait = false;
  bool quiet = false;
  const char *mode = "user";
  uint32_t send_mode = IPI_MODE_SMP;

  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    if (strcmp(arg, "-t") == 0 || strcmp(arg, "--target") == 0) {
      if (i + 1 >= argc) {
        usage(argv[0]);
        return 1;
      }
      target_cpu = atoi(argv[++i]);
      continue;
    }
    if (strcmp(arg, "-s") == 0 || strcmp(arg, "--sender") == 0) {
      if (i + 1 >= argc) {
        usage(argv[0]);
        return 1;
      }
      sender_cpu = atoi(argv[++i]);
      continue;
    }
    if (strcmp(arg, "-r") == 0 || strcmp(arg, "--rate") == 0) {
      if (i + 1 >= argc) {
        usage(argv[0]);
        return 1;
      }
      rate = atof(argv[++i]);
      continue;
    }
    if (strcmp(arg, "-D") == 0 || strcmp(arg, "--duration") == 0) {
      if (i + 1 >= argc) {
        usage(argv[0]);
        return 1;
      }
      duration = atof(argv[++i]);
      continue;
    }
    if (strcmp(arg, "-d") == 0 || strcmp(arg, "--device") == 0) {
      if (i + 1 >= argc) {
        usage(argv[0]);
        return 1;
      }
      device = argv[++i];
      continue;
    }
    if (strcmp(arg, "-w") == 0 || strcmp(arg, "--wait") == 0) {
      wait = true;
      continue;
    }
    if (strcmp(arg, "-m") == 0 || strcmp(arg, "--mode") == 0) {
      if (i + 1 >= argc) {
        usage(argv[0]);
        return 1;
      }
      mode = argv[++i];
      if (strcmp(mode, "apic") == 0) {
        send_mode = IPI_MODE_APIC;
      } else {
        send_mode = IPI_MODE_SMP;
      }
      continue;
    }
    if (strcmp(arg, "-q") == 0 || strcmp(arg, "--quiet") == 0) {
      quiet = true;
      continue;
    }
    if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
      usage(argv[0]);
      return 0;
    }

    fprintf(stderr, "未知参数: %s\n", arg);
    usage(argv[0]);
    return 1;
  }

  if (target_cpu < 0) {
    fprintf(stderr, "必须指定目标 CPU\n");
    usage(argv[0]);
    return 1;
  }
  if (strcmp(mode, "timer") == 0) {
    if (sender_cpu < 0) {
      sender_cpu = target_cpu;
    }
  } else if (sender_cpu >= 0) {
    if (pin_to_cpu(sender_cpu) != 0) {
      fprintf(stderr, "绑定发送端 CPU 失败: %s\n", strerror(errno));
      return 1;
    }
  }

  int fd = open(device, O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "打开设备失败 %s: %s\n", device, strerror(errno));
    return 1;
  }

  uint64_t start_ns = now_ns();
  uint64_t end_ns = 0;
  if (duration > 0.0) {
    end_ns = start_ns + (uint64_t)(duration * 1000000000.0);
  }
  uint64_t send_count = 0;
  uint64_t handled_count = 0;

  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);

  if (strcmp(mode, "user") == 0) {
    struct timespec interval = {0};
    if (rate > 0.0) {
      double interval_ns = 1000000000.0 / rate;
      interval.tv_sec = (time_t)(interval_ns / 1000000000.0);
      interval.tv_nsec = (long)(interval_ns - interval.tv_sec * 1000000000.0);
    }

    struct ipi_request req;
    req.cpu = (uint32_t)target_cpu;
    req.wait = wait ? 1u : 0u;
    req.mode = send_mode;

    while (!g_stop) {
      uint64_t now = now_ns();
      if (end_ns != 0 && now >= end_ns) {
        break;
      }
      if (ioctl(fd, IPI_IOC_SEND, &req) != 0) {
        fprintf(stderr, "ioctl 失败: %s\n", strerror(errno));
        close(fd);
        return 1;
      }
      send_count++;
      if (rate > 0.0) {
        nanosleep(&interval, NULL);
      }
    }
    handled_count = send_count;
  } else if (strcmp(mode, "kthread") == 0 || strcmp(mode, "flood") == 0 ||
             strcmp(mode, "apic") == 0) {
    struct ipi_flood_request req;
    req.cpu = (uint32_t)target_cpu;
    req.sender = (sender_cpu < 0) ? IPI_CPU_ANY : (uint32_t)sender_cpu;
    req.wait = wait ? 1u : 0u;
    req.rate = (rate > 0.0) ? (uint32_t)rate : 0u;
    req.mode = send_mode;
    if (ioctl(fd, IPI_IOC_START_FLOOD, &req) != 0) {
      fprintf(stderr, "ioctl 启动 flood 失败: %s\n", strerror(errno));
      close(fd);
      return 1;
    }
    struct timespec idle = {0, 1000000};
    while (!g_stop) {
      uint64_t now = now_ns();
      if (end_ns != 0 && now >= end_ns) {
        break;
      }
      nanosleep(&idle, NULL);
    }
    struct ipi_count out = {0};
    if (ioctl(fd, IPI_IOC_STOP_FLOOD, &out) != 0) {
      fprintf(stderr, "ioctl 停止 flood 失败: %s\n", strerror(errno));
      close(fd);
      return 1;
    }
    send_count = out.sent;
    handled_count = out.handled;
  } else if (strcmp(mode, "timer") == 0) {
    struct ipi_timer_request req;
    req.cpu = (uint32_t)target_cpu;
    req.rate = (rate > 0.0) ? (uint32_t)rate : 0u;
    if (req.rate == 0) {
      fprintf(stderr, "timer 模式必须指定正的 --rate\n");
      close(fd);
      return 1;
    }
    if (ioctl(fd, IPI_IOC_START_TIMER, &req) != 0) {
      fprintf(stderr, "ioctl 启动 timer 失败: %s\n", strerror(errno));
      close(fd);
      return 1;
    }
    struct timespec idle = {0, 1000000};
    while (!g_stop) {
      uint64_t now = now_ns();
      if (end_ns != 0 && now >= end_ns) {
        break;
      }
      nanosleep(&idle, NULL);
    }
    struct ipi_count out = {0};
    if (ioctl(fd, IPI_IOC_STOP_TIMER, &out) != 0) {
      fprintf(stderr, "ioctl 停止 timer 失败: %s\n", strerror(errno));
      close(fd);
      return 1;
    }
    send_count = out.sent;
    handled_count = out.handled;
  } else {
    fprintf(stderr, "未知 mode: %s\n", mode);
    close(fd);
    return 1;
  }

  if (!quiet) {
    double elapsed = (double)(now_ns() - start_ns) / 1e9;
    double actual_rate =
        elapsed > 0.0 ? (double)send_count / elapsed : 0.0;
    fprintf(stderr,
            "[ipi_sender] mode=%s target=%d sender=%d count=%" PRIu64
            " handled=%" PRIu64 " elapsed=%.3f rate=%.1f wait=%d\n",
            mode, target_cpu, sender_cpu, send_count, handled_count,
            elapsed, actual_rate, wait ? 1 : 0);
  }

  close(fd);
  return 0;
}
