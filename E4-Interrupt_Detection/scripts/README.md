# scripts 目录说明

本目录包含若干用于构建、插桩与验证的脚本。以下为每个脚本的用途与使用方法。

## demo.sh

用途：
- 原始示例：使用 `auto-instrument` 插入内联汇编（默认 `nop`），验证基础插桩流程。

使用：
```
./scripts/demo.sh
```

## intmon_demo.sh

用途：
- 条件分支插桩演示：生成插桩后的 IR，并展示 `__intmon_prepare/__intmon_check` 的位置。

使用：
```
./scripts/intmon_demo.sh
```

## intmon_run.sh

用途：
- 一键编译 → 插桩 → 链接运行时（TLS 模式）→ 运行输出。
 - 程序退出时会打印耗时统计（总耗时与“包含插桩函数”的总耗时）。

使用：
```
./scripts/intmon_run.sh
```

自定义输入/输出/运行时：
```
./scripts/intmon_run.sh -i /path/to/foo.c -o /tmp/foo.intmon -r runtime/intmon_runtime.c
```

## intmon_run_gs.sh

用途：
- GS 模式运行：编译时自动加 `-DINTMON_USE_GS`，使用段寄存器方案。
- 适用于真实 x86 环境；容器/虚拟化环境可能失败。

使用：
```
./scripts/intmon_run_gs.sh
```

## secret_demo.sh

用途：
- secret 模式演示：自动生成 `examples/secret_demo.c`，以 `-instrument-mode=secret` 插桩并展示结果。

使用：
```
./scripts/secret_demo.sh
```

## divergence_demo.sh

用途：
- 控制流差异点演示：开启 `-divergence=on`，插入并展示 `__intmon_div`。

使用：
```
./scripts/divergence_demo.sh
```

## known_cases.sh

用途：
- 一键实测已知用例：下载/编译四个指定版本源码并按指定行号插桩。
- 目标：MbedTLS 2.6.1/3.6.1、WolfSSL 5.7.2、Libjpeg 9f。
- 会编译并执行每个用例的最小 `main`，打印插桩后的运行输出。

使用：
```
./scripts/known_cases.sh
```

跳过下载或构建：
```
./scripts/known_cases.sh --skip-download --skip-build
```

指定 __intmon_check 插入位置：
```
./scripts/known_cases.sh --f2-at=join
```

使用 GS 模式运行（仅适用于真实 x86_64 Linux 环境；容器/虚拟化可能失败）：
```
./scripts/known_cases.sh --use-gs
```

GS 模式下将 prepare/check 内联为汇编指令（需配合 --use-gs）：
```
./scripts/known_cases.sh --use-gs --inline-gs
```

启用 IPI 中断打断（需要内核模块，默认自动选择空闲核心并绑定）：
```
./scripts/known_cases.sh --use-gs --with-ipi
```

可选参数：
```
--ipi-rate=20000         # IPI 发送频率（次/秒）
--ipi-duration=2         # 发送时长（秒），auto 表示覆盖 B 运行周期
--ipi-target-cpu=0       # B 程序绑定核心
--ipi-sender-cpu=1       # 发送端绑定核心（需与 target 不同）
--ipi-warmup-ms=50       # 发送端启动后等待时间
--ipi-device=/dev/ipi_ctl
--ipi-wait               # 等待 IPI 回调完成（默认异步）
```

## remote_test_pku_sgx.sh

用途：
- 将本项目同步到远程服务器 pku-sgx，并在远程执行 `known_cases.sh`。
- 默认同步到：`/home/chen/GitProject/<项目名>`。

使用：
```
./scripts/remote_test_pku_sgx.sh -- --skip-download --skip-build --run-modes=both
```

常用选项：
```
./scripts/remote_test_pku_sgx.sh --with-third-party -- --skip-download --skip-build
./scripts/remote_test_pku_sgx.sh --remote-base=/home/chen/GitProject -- --run-modes=api
./scripts/remote_test_pku_sgx.sh --with-llvm -- --skip-download --run-modes=both
./scripts/remote_test_pku_sgx.sh --with-llvm-force -- --skip-download --run-modes=both
./scripts/remote_test_pku_sgx.sh --llvm-tarball=/path/to/llvm.tar.xz --llvm-prefix=/home/chen/GitProject/segscope/llvm-18 -- --run-modes=core
```

说明：
 - 使用 `--with-llvm` 时，远程会自动使用 `build-linux-llvm` 作为构建目录，避免旧的 CMakeCache 误用系统 LLVM。
 - 使用 `--with-llvm-force` 时会强制覆盖远程 LLVM 目录（会删除远程 LLVM_PREFIX 目录）。

## remote_sync_pku_sgx.sh

用途：
- 仅同步代码到远程服务器 pku-sgx（不执行任何测试）。
- 适合手动同步一次或快速更新脚本/内核模块。

使用：
```
./scripts/remote_sync_pku_sgx.sh
```

常用选项：
```
./scripts/remote_sync_pku_sgx.sh --with-third-party
./scripts/remote_sync_pku_sgx.sh --with-build --delete
```

## remote_pull_pku_sgx.sh

用途：
- 将远程结果目录拉回本地（支持只拉回 tsv/summary）。
- 默认匹配：`build-linux-llvm.ipi-*.tsc`。

使用：
```
./scripts/remote_pull_pku_sgx.sh
```

常用选项：
```
./scripts/remote_pull_pku_sgx.sh --only-results
./scripts/remote_pull_pku_sgx.sh --only-tsv
./scripts/remote_pull_pku_sgx.sh --only-summary
./scripts/remote_pull_pku_sgx.sh --pattern=build-linux-llvm.ipi-*.tsc --dest=/tmp/intr_results
```

## remote_test_pku_sgx_fetch.sh

用途：
- 远程执行 `known_cases.sh`，并自动拉回结果到本地。

使用：
```
./scripts/remote_test_pku_sgx_fetch.sh --with-llvm --with-third-party -- --skip-download --run-modes=both --secret-instrument=both
```

说明：
 - 默认拉回 `known_cases_results*.tsv` 与 `known_cases_summary*.md`。
 - 本地文件名会自动加上主机名（如 `pku-sgx`）以避免覆盖。
 - 无参数时等价于上述命令。
 - 可用 `--fetch-only` 仅拉回结果；`--no-llvm` 禁用默认传 LLVM；`--gs` 透传为 `known_cases.sh --use-gs`。

## ipi_build_kmod.sh

用途：
- 构建 IPI 内核模块（用于跨核中断打断测试）。

使用：
```
./scripts/ipi_build_kmod.sh
sudo insmod kernel/ipi_kmod/ipi_kmod.ko
sudo chmod 666 /dev/ipi_ctl
```

## ipi_module.sh

用途：
- 一键构建/安装/卸载/检查 IPI 内核模块。

使用：
```
./scripts/ipi_module.sh build
./scripts/ipi_module.sh install
./scripts/ipi_module.sh status
./scripts/ipi_module.sh uninstall
```

## E4 顶层一键入口

若按 E4 目录组织实验，可直接执行：

```
./E4-Interrupt_Detection/run_all.sh
```

说明：

- `E4-Interrupt_Detection/run_all.sh` 是 E4 顶层一键脚本
- 底层仍调用本目录中的 `run_all.sh`
- 结果默认写入 `E4-Interrupt_Detection/results/`
- 共享构建产物默认写入 `E4-Interrupt_Detection/build-base/`
- 默认运行 `100` 次外层重复（每个程序内部默认循环 `100` 次）
- 会在开头自动检查 LLVM 18 / clang 18 / cmake；若缺失，则自动下载官方 LLVM 18.1.8 预编译包到 `E4-Interrupt_Detection/llvm-18/`；待测程序源码首轮自动下载
- 输出日志与 TSV 会显式包含 `library/version/algorithm/target_branch/instrument_mode/ipi_mode/run_mode` 等字段，便于后续整理实验总表
