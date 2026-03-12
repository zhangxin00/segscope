#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/smp.h>
#include <linux/cpu.h>
#include <linux/kthread.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/io.h>
#include <asm/msr.h>
#include <asm/msr-index.h>
#include <asm/apicdef.h>
#include <asm/irq_vectors.h>

#define IPI_IOC_MAGIC 'i'
#define IPI_IOC_SEND _IOW(IPI_IOC_MAGIC, 1, struct ipi_request)
#define IPI_IOC_START_FLOOD _IOW(IPI_IOC_MAGIC, 2, struct ipi_flood_request)
#define IPI_IOC_STOP_FLOOD _IOR(IPI_IOC_MAGIC, 3, struct ipi_count)
#define IPI_IOC_START_TIMER _IOW(IPI_IOC_MAGIC, 4, struct ipi_timer_request)
#define IPI_IOC_STOP_TIMER _IOR(IPI_IOC_MAGIC, 5, struct ipi_count)
#define IPI_CPU_ANY 0xFFFFFFFFu
#define IPI_MODE_SMP 0u
#define IPI_MODE_APIC 1u

#ifndef MSR_IA32_X2APIC_ICR
#define MSR_IA32_X2APIC_ICR 0x830
#endif
#ifndef APIC_INT_ASSERT
#define APIC_INT_ASSERT 0x04000
#endif

struct ipi_request {
  __u32 cpu;
  __u32 wait;
  __u32 mode;
};

struct ipi_flood_request {
  __u32 cpu;
  __u32 sender;
  __u32 wait;
  __u32 rate;
  __u32 mode;
};

struct ipi_timer_request {
  __u32 cpu;
  __u32 rate;
};

struct ipi_count {
  __u64 sent;
  __u64 handled;
};

static int ipi_send_apic(__u32 cpu, __u32 wait);

static DEFINE_MUTEX(flood_lock);
static struct task_struct *flood_task;
static atomic64_t flood_send_count;
static atomic64_t flood_handled_count;
static struct ipi_flood_request flood_req;

static DEFINE_MUTEX(timer_lock);
static struct task_struct *timer_task;
static atomic64_t timer_count;
static struct ipi_timer_request timer_req;
static struct hrtimer local_timer;
static u64 timer_interval_ns;
static void __iomem *apic_mmio;

static void ipi_nop(void *info) {
  (void)info;
  atomic64_inc(&flood_handled_count);
}

static int ipi_flood_thread(void *data) {
  u64 interval_ns = 0;
  u64 next_ns = 0;
  if (flood_req.rate > 0) {
    interval_ns = (u64)NSEC_PER_SEC / flood_req.rate;
    if (interval_ns == 0) {
      interval_ns = 1;
    }
    next_ns = ktime_get_ns();
  }
  while (!kthread_should_stop()) {
    if (flood_req.mode == IPI_MODE_APIC) {
      ipi_send_apic(flood_req.cpu, flood_req.wait);
    } else {
      smp_call_function_single(flood_req.cpu, ipi_nop, NULL,
                               flood_req.wait ? 1 : 0);
    }
    atomic64_inc(&flood_send_count);
    if (interval_ns > 0) {
      next_ns += interval_ns;
      while (!kthread_should_stop() && ktime_get_ns() < next_ns) {
        cpu_relax();
      }
    } else {
      cpu_relax();
    }
    cond_resched();
  }
  return 0;
}

static bool ipi_x2apic_enabled(void) {
  u64 apic_base = 0;
  rdmsrl(MSR_IA32_APICBASE, apic_base);
  return (apic_base & X2APIC_ENABLE) != 0;
}

static int ipi_map_apic_mmio(void) {
  u64 apic_base = 0;
  u64 phys = 0;
  if (apic_mmio) {
    return 0;
  }
  rdmsrl(MSR_IA32_APICBASE, apic_base);
  phys = apic_base & MSR_IA32_APICBASE_BASE;
  if (!phys) {
    return -EINVAL;
  }
  apic_mmio = ioremap(phys, 0x1000);
  if (!apic_mmio) {
    return -ENOMEM;
  }
  return 0;
}

static int ipi_send_apic(__u32 cpu, __u32 wait) {
  u32 apicid = cpu_physical_id(cpu);
  u32 vector = CALL_FUNCTION_SINGLE_VECTOR;
  if (ipi_x2apic_enabled()) {
    u64 icr = ((u64)apicid << 32) |
              (u64)(vector | APIC_DM_FIXED | APIC_DEST_PHYSICAL |
                    APIC_INT_ASSERT);
    wrmsrl(MSR_IA32_X2APIC_ICR, icr);
    if (wait) {
      u64 val = 0;
      int loops = 0;
      do {
        rdmsrl(MSR_IA32_X2APIC_ICR, val);
        if ((val & APIC_ICR_BUSY) == 0) {
          break;
        }
        cpu_relax();
      } while (++loops < 1000000);
    }
    return 0;
  }
  if (ipi_map_apic_mmio() != 0) {
    return -ENODEV;
  }
  writel(apicid << 24, apic_mmio + APIC_ICR2);
  writel(vector | APIC_DM_FIXED | APIC_DEST_PHYSICAL | APIC_INT_ASSERT,
         apic_mmio + APIC_ICR);
  if (wait) {
    int loops = 0;
    while (loops++ < 1000000) {
      u32 low = readl(apic_mmio + APIC_ICR);
      if ((low & APIC_ICR_BUSY) == 0) {
        break;
      }
      cpu_relax();
    }
  }
  return 0;
}

static enum hrtimer_restart ipi_timer_cb(struct hrtimer *timer) {
  atomic64_inc(&timer_count);
  hrtimer_forward_now(timer, ns_to_ktime(timer_interval_ns));
  return HRTIMER_RESTART;
}

static int ipi_timer_thread(void *data) {
  hrtimer_init(&local_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
  local_timer.function = ipi_timer_cb;
  hrtimer_start(&local_timer, ns_to_ktime(timer_interval_ns),
                HRTIMER_MODE_REL_PINNED);
  while (!kthread_should_stop()) {
    set_current_state(TASK_INTERRUPTIBLE);
    schedule();
  }
  __set_current_state(TASK_RUNNING);
  hrtimer_cancel(&local_timer);
  return 0;
}

static long ipi_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
  struct ipi_request req;
  struct ipi_flood_request freq;
  struct ipi_timer_request treq;
  struct ipi_count cnt;
  int ret;

  switch (cmd) {
    case IPI_IOC_SEND:
      if (copy_from_user(&req, (void __user *)arg, sizeof(req)) != 0) {
        pr_err("[ipi_kmod] SEND: copy_from_user 失败\n");
        return -EFAULT;
      }
      if (req.cpu >= nr_cpu_ids || !cpu_online(req.cpu)) {
        pr_err("[ipi_kmod] SEND: 非法 cpu=%u (nr_cpu_ids=%u online=%d)\n",
               req.cpu, nr_cpu_ids,
               req.cpu < nr_cpu_ids ? cpu_online(req.cpu) : 0);
        return -EINVAL;
      }
      if (req.mode == IPI_MODE_APIC) {
        ret = ipi_send_apic(req.cpu, req.wait);
        if (ret != 0) {
          pr_err("[ipi_kmod] SEND: APIC 发送失败 cpu=%u ret=%d\n",
                 req.cpu, ret);
        }
        return ret;
      }
      return smp_call_function_single(req.cpu, ipi_nop, NULL,
                                      req.wait ? 1 : 0);
    case IPI_IOC_START_FLOOD:
      if (copy_from_user(&freq, (void __user *)arg, sizeof(freq)) != 0) {
        pr_err("[ipi_kmod] START_FLOOD: copy_from_user 失败\n");
        return -EFAULT;
      }
      if (freq.cpu >= nr_cpu_ids || !cpu_online(freq.cpu)) {
        pr_err("[ipi_kmod] START_FLOOD: 非法 cpu=%u (nr_cpu_ids=%u online=%d)\n",
               freq.cpu, nr_cpu_ids,
               freq.cpu < nr_cpu_ids ? cpu_online(freq.cpu) : 0);
        return -EINVAL;
      }
      if (freq.sender != IPI_CPU_ANY &&
          (freq.sender >= nr_cpu_ids || !cpu_online(freq.sender))) {
        pr_err("[ipi_kmod] START_FLOOD: 非法 sender=%u (nr_cpu_ids=%u online=%d)\n",
               freq.sender, nr_cpu_ids,
               freq.sender < nr_cpu_ids ? cpu_online(freq.sender) : 0);
        return -EINVAL;
      }
      mutex_lock(&flood_lock);
      if (flood_task) {
        mutex_unlock(&flood_lock);
        pr_err("[ipi_kmod] START_FLOOD: 已在运行 cpu=%u sender=%u\n",
               freq.cpu, freq.sender);
        return -EBUSY;
      }
      if (freq.mode != IPI_MODE_APIC) {
        freq.mode = IPI_MODE_SMP;
      }
      flood_req = freq;
      atomic64_set(&flood_send_count, 0);
      atomic64_set(&flood_handled_count, 0);
      flood_task = kthread_create(ipi_flood_thread, NULL, "ipi_flood");
      if (IS_ERR(flood_task)) {
        ret = PTR_ERR(flood_task);
        flood_task = NULL;
        mutex_unlock(&flood_lock);
        pr_err("[ipi_kmod] START_FLOOD: kthread_create 失败 ret=%d\n", ret);
        return ret;
      }
      if (flood_req.sender != IPI_CPU_ANY) {
        kthread_bind(flood_task, flood_req.sender);
      }
      wake_up_process(flood_task);
      mutex_unlock(&flood_lock);
      return 0;
    case IPI_IOC_STOP_FLOOD:
      mutex_lock(&flood_lock);
      if (!flood_task) {
        mutex_unlock(&flood_lock);
        pr_err("[ipi_kmod] STOP_FLOOD: 未在运行\n");
        return -EINVAL;
      }
      kthread_stop(flood_task);
      flood_task = NULL;
      cnt.sent = (u64)atomic64_read(&flood_send_count);
      cnt.handled = (u64)atomic64_read(&flood_handled_count);
      mutex_unlock(&flood_lock);
      if (copy_to_user((void __user *)arg, &cnt, sizeof(cnt)) != 0) {
        pr_err("[ipi_kmod] STOP_FLOOD: copy_to_user 失败\n");
        return -EFAULT;
      }
      return 0;
    case IPI_IOC_START_TIMER:
      if (copy_from_user(&treq, (void __user *)arg, sizeof(treq)) != 0) {
        pr_err("[ipi_kmod] START_TIMER: copy_from_user 失败\n");
        return -EFAULT;
      }
      if (treq.cpu >= nr_cpu_ids || !cpu_online(treq.cpu)) {
        pr_err("[ipi_kmod] START_TIMER: 非法 cpu=%u (nr_cpu_ids=%u online=%d)\n",
               treq.cpu, nr_cpu_ids,
               treq.cpu < nr_cpu_ids ? cpu_online(treq.cpu) : 0);
        return -EINVAL;
      }
      if (treq.rate == 0) {
        pr_err("[ipi_kmod] START_TIMER: 非法 rate=0\n");
        return -EINVAL;
      }
      timer_interval_ns = (u64)NSEC_PER_SEC / treq.rate;
      if (timer_interval_ns == 0) {
        timer_interval_ns = 1;
      }
      mutex_lock(&timer_lock);
      if (timer_task) {
        mutex_unlock(&timer_lock);
        pr_err("[ipi_kmod] START_TIMER: 已在运行 cpu=%u rate=%u\n",
               treq.cpu, treq.rate);
        return -EBUSY;
      }
      timer_req = treq;
      atomic64_set(&timer_count, 0);
      timer_task = kthread_create(ipi_timer_thread, NULL, "ipi_timer");
      if (IS_ERR(timer_task)) {
        ret = PTR_ERR(timer_task);
        timer_task = NULL;
        mutex_unlock(&timer_lock);
        pr_err("[ipi_kmod] START_TIMER: kthread_create 失败 ret=%d\n", ret);
        return ret;
      }
      kthread_bind(timer_task, timer_req.cpu);
      wake_up_process(timer_task);
      mutex_unlock(&timer_lock);
      return 0;
    case IPI_IOC_STOP_TIMER:
      mutex_lock(&timer_lock);
      if (!timer_task) {
        mutex_unlock(&timer_lock);
        pr_err("[ipi_kmod] STOP_TIMER: 未在运行\n");
        return -EINVAL;
      }
      kthread_stop(timer_task);
      timer_task = NULL;
      cnt.sent = (u64)atomic64_read(&timer_count);
      cnt.handled = cnt.sent;
      mutex_unlock(&timer_lock);
      if (copy_to_user((void __user *)arg, &cnt, sizeof(cnt)) != 0) {
        pr_err("[ipi_kmod] STOP_TIMER: copy_to_user 失败\n");
        return -EFAULT;
      }
      return 0;
    default:
      return -EINVAL;
  }
}

static const struct file_operations ipi_fops = {
  .owner = THIS_MODULE,
  .unlocked_ioctl = ipi_ioctl,
#ifdef CONFIG_COMPAT
  .compat_ioctl = ipi_ioctl,
#endif
};

static struct miscdevice ipi_dev = {
  .minor = MISC_DYNAMIC_MINOR,
  .name = "ipi_ctl",
  .fops = &ipi_fops,
};

static int __init ipi_init(void) {
  int ret = misc_register(&ipi_dev);
  if (ret != 0) {
    pr_err("[ipi_kmod] 注册失败: %d\n", ret);
    return ret;
  }
  pr_info("[ipi_kmod] 已注册 /dev/ipi_ctl\n");
  return 0;
}

static void __exit ipi_exit(void) {
  mutex_lock(&flood_lock);
  if (flood_task) {
    kthread_stop(flood_task);
    flood_task = NULL;
  }
  mutex_unlock(&flood_lock);
  mutex_lock(&timer_lock);
  if (timer_task) {
    kthread_stop(timer_task);
    timer_task = NULL;
  }
  mutex_unlock(&timer_lock);
  if (apic_mmio) {
    iounmap(apic_mmio);
    apic_mmio = NULL;
  }
  misc_deregister(&ipi_dev);
  pr_info("[ipi_kmod] 已卸载\n");
}

module_init(ipi_init);
module_exit(ipi_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("intr_detect");
MODULE_DESCRIPTION("minimal ipi sender control device");
