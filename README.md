# rt-health：实时核负载与资源干扰监测

rt-health 用于监测 Xenomai 4（EVL）系统中隔离实时核的运行状态。它综合硬件性能计数器（缓存未命中、TLB 未命中、IPC、后端停顿）、系统统计（利用率、中断、频率、温度）和实时任务自身上报的帧耗时，在实时环境即将过载时提前告警。

## 1. 设计依据

### 1.1 预警需要两类信号

- **事实信号**：实时任务自己报告的帧耗时与超周期次数。只有它能确定地说明“实时性已经受损”，因此只有它能把状态升为 FAIL；
- **趋势信号**：缓存、TLB、IPC、停顿、中断等硬件统计。它们不直接说明超时，但通常在超时之前就开始变化，用于提前告警（WARN）。

### 1.2 不看绝对值，看相对基线

缺失率的绝对值本身不能说明问题。顺序扫描大数组的程序，末级缓存（LLC）几乎每次访问都会未命中，但它照样能跑满内存带宽，效率并不低。所以 rt-health：

1. 把计数换算为**每千条指令的未命中数（MPKI）**，与任务的执行速度无关；
2. 为每个核**学习自己的正常范围**：保留最近 120 个采样，以中位数和 MAD（中位数绝对偏差）估计基线和离散度；
3. 当前值偏离基线超过 3.5 个稳健标准差，并超过一个绝对下限时，才告警。

使用中位数和 MAD，而不是均值和标准差，是为了避免一次尖峰拉宽正常范围，导致之后再也不告警。

### 1.3 监测本身不能干扰实时核

- **只计数，不采样。** 在 Dovetail 内核上，PMU 溢出中断（PMI）通过 NMI 处理函数 `perf_event_nmi_handler()` 送达（`arch/x86/events/core.c`，注册为 `NMI_LOCAL`）。NMI 不受中断流水线管控，会直接打断实时线程。计数模式在读取之前不产生任何中断。
- **每个采样周期只发一次 IPI。** 读取一个处于活动状态的核级 perf 事件，要在该事件所属的核上完成：内核经 `perf_event_read()` → `smp_call_function_single()` 向被监测的核发送 IPI（`kernel/events/core.c`）；停止、启动事件也各需一次 IPI。rt-health 把全部事件放在一个组内，以 `PERF_FORMAT_GROUP` 对组长做一次 `read()` 取回所有值，每核每个采样周期只有一次 IPI。在 Dovetail 内核上这是一个带内中断：实时线程运行时它只被记入日志，等该核回到 Linux 时才执行，因此不会抢占实时线程，只付出一次流水线入口开销。
- **监测进程运行在非实时核上**，用 `taskset` 绑定即可。

## 2. 实时核过载的成因与对应指标

| 成因 | 机理 | 主要指标 |
|---|---|---|
| 计算密度过高 | 单帧计算量接近帧周期，余量消失 | 帧耗时上升、超周期；利用率接近 100% |
| 工作集超出缓存 | 每帧访问的数据超过 L2/LLC 容量，或多个实时任务的工作集相互挤出 | LLC MPKI 上升，IPC 下降 |
| 共享 LLC 与内存带宽争用 | 非实时核上的负载占用共享 LLC 和内存带宽（“吵闹的邻居”）。实时核虽然隔离，硬件资源仍然共享 | 实时核的 LLC MPKI 和后端停顿上升，但其自身代码没有变化 |
| 访问模式不友好 | 随机访问、跨步访问、指针追逐，使硬件预取失效 | LLC MPKI 和后端停顿上升 |
| TLB 覆盖不足 | 工作集跨越的 4 KiB 页太多，超出 TLB 覆盖范围 | dTLB MPKI 上升。对策：hugetlbfs 大页 |
| 远端 NUMA 内存 | 线程运行的节点与内存所在节点不同，访问延迟增加 | 只在多 NUMA 节点机器上存在；表现为后端停顿上升。可用 `numastat -p` 确认 |
| 伪共享 | 两个核写同一缓存行，缓存行在核间来回传递 | 后端停顿上升，IPC 下降 |
| 中断落到实时核 | 设备中断或 IPI 被分配到实时核 | 实时核上出现设备类中断（不需要基线，第一个采样即告警，并给出 IRQ 号与设备名）；IPI 频率异常 |
| 降级到 Linux | 普通系统调用、缺页、信号使实时线程离开带外阶段 | 线程模式下的 context switch、page fault 计数（`-p`） |
| 频率与温控 | 温控降频、调频策略被改回节能 | 实时核频率下降、温度上升 |
| SMI | 固件的系统管理中断，所有核暂停 | 帧耗时偶发尖峰；需用 `rdmsr 0x34` 单独检查 |

### 关于 NUMA 的说明

远端 NUMA 内存只在**多个 NUMA 节点**的机器上才会出现（多路服务器，或开启了 SNC 等子 NUMA 划分的单路服务器）。本研究使用的笔记本（Intel Core Ultra 9 285H）只有一个 NUMA 节点，不存在远端内存。在多节点机器上，还要注意内核的 NUMA 自动均衡会改写锁定页的页表项、引发缺页并导致降级，应关闭它（`numa_balancing=disable`）。

### 关于“缓存效率低”的说明

缓存效率低不能简单地由未命中率判断，原因见 1.2 节。更可靠的判断方法是：同一个任务的 MPKI 相对于它自己的基线上升，**同时** IPC 下降，**而且**帧耗时随之增加。只有三者同时出现，才能确认是缓存问题拖慢了任务。

## 3. 编译与使用

```
make
sudo ./rt-health                 # 默认监测 /sys/devices/system/cpu/isolated 中的核
sudo ./rt-health -c 4,5 -i 1000 -o rt.csv
sudo ./rt-health -p <tid>        # 跟踪一个实时线程，同时报告降级相关的软件事件
./rt-health -c 4 -t /dev/shm/rt-telemetry.bin   # 读取实时任务上报的帧数据
```

| 选项 | 含义 |
|---|---|
| `-c 4,5` / `-c 4-7` / `-c all` | 监测的核，默认取隔离核 |
| `-p tid` | 跟踪一个线程，替代 `-c` |
| `-i ms` | 采样间隔，默认 1000 ms |
| `-n count` | 采样次数，默认一直运行 |
| `-o file.csv` | 同时输出 CSV |
| `-t file` | 实时任务的帧遥测共享文件 |
| `-graphic`（或 `-g`、`--graphic`） | 全屏图形界面，见下文 |

建议把监测进程绑定到非实时核：`taskset -c 0 sudo ./rt-health ...`。

### 图形界面（-graphic）

```
sudo taskset -c 0 ./rt-health -c 4,5 -i 500 -graphic
```

类似 htop 的全屏界面。被监测的核**横向并排**，每核一列；每列内各项指标**纵向排列**，每项是一个数行高的滚动柱状图，每个采样一根柱，最新的在最右：

- **▲ 向上生长：越高越好**：IPC；
- **▼ 向下生长（从顶部垂下）：越低越好**：LLC 未命中/K、dTLB 未命中/K、设备中断/s、非定时器中断/s（温度事件 + IPI + 设备），以及使用 `-t` 时的最坏帧耗时；
- **■ CPU 忙碌率**：向上生长，本身无好坏；
- **颜色**：每根柱按该采样的状态着色，绿色 OK、黄色 WARN、红色 FAIL；
- **刻度固定**，同样高度始终代表同样的值；未命中率与中断为对数刻度，左侧标出起点和终点的刻度值；
- **面板头部**：状态、忙碌率（同时给出 Linux 看到的利用率作对比）、频率、温度、各类中断的每秒次数，WARN/FAIL 时给出原因；
- **事件栏**（底部）：状态变化的时间和原因，最新的在上，原因类型不变时不重复记录；
- **按键**：`q` 退出，`p` 暂停刷新（采样和基线学习继续），`c` 清空事件栏。

终端行数不够时，按重要性减少每核显示的图表。需要 UTF-8 终端。`-o` 可以同时使用。

### CPU 忙碌率（busy%）

`/proc/stat` 是 Linux 自己的统计，看不到带外（EVL）线程的执行时间：运行 evl-pingpong 的核上它显示 0%。rt-health 改用硬件计数：核心非空闲时的参考周期数（`ref-cycles`，x86 上以 TSC 频率计数，停机时不计数）除以“参考频率 × 采样间隔”。参考频率在启动时用一次 0.2 s 的自测量标定。

实机核对（ThinkBook，CPU 4、5）：

| 负载 | busy% | Linux 看到的利用率 | 预期 |
|---|---:|---:|---|
| 空闲 | 0.6–0.7 | 0.0 | 约 0（仅定时器） |
| evl-pingpong | 2.3–3.0 | 0.0 | ≥ 1.6（A 线程每 1 ms 忙碌约 16 µs，加中断与切换开销） |
| 占空比 50% 的 Linux 任务 | 55.8 | 50.5 | 约 50，加该任务自身的定时唤醒 |

基线学习只接受 busy% > 1.5% 的采样（空闲核 0.6–0.7%，运行 pingpong 的核 2–3%）。此前按 Linux 利用率 > 3% 判断，只运行 EVL 线程的核永远不会被当作“忙碌”，缓存与 IPC 检查在这些核上实际从未生效；改用 busy% 后已修正。

### 输出列

| 列 | 含义 |
|---|---|
| state | OK / WARN（趋势信号偏离基线）/ FAIL（实时任务报告超周期） |
| busy% | 硬件测得的忙碌率，包含 EVL 线程（见下文） |
| util% | Linux 看到的利用率（核模式取 `/proc/stat`，线程模式取 task-clock），看不到 EVL 线程 |
| IPC | 每周期指令数 |
| LLC/K、dTLB/K、cache/K | 每千条指令的 LLC 读未命中、dTLB 读未命中、缓存未命中 |
| stall% | 后端停顿周期占比（许多 Intel 处理器不提供这一通用事件，显示为 -） |
| IRQ/s | 该核每秒收到的中断数 |

### 权限与环境

- 核级计数需要 root，或 `perf_event_paranoid` ≤ 0；
- 非 root 且 `perf_event_paranoid` = 2 时，只能计数自己的进程，并且只计用户态。rt-health 会自动降级到这一模式；
- 虚拟机（包括 WSL2）通常不提供硬件 PMU。此时 rt-health 只用软件事件，缓存与 TLB 列显示为 -；
- 混合架构处理器（如 Core Ultra）的 P 核和 E 核有不同的 PMU（`cpu_core`、`cpu_atom`）。本程序使用通用事件，由内核按核类型映射。

## 4. 实时任务接入帧遥测

帧遥测是事实信号的来源。实时任务每帧调用一次 `rft_publish()`，只有普通内存写和一次原子写，不进入内核，不会降级（`telemetry/rt_frames.h`）：

```c
#include "rt_frames.h"

/* 初始化期（Linux 态） */
int fd = open("/dev/shm/rt-telemetry.bin", O_RDWR | O_CREAT, 0644);
ftruncate(fd, sizeof(struct rft_page));
struct rft_page *pg = mmap(NULL, sizeof(*pg), PROT_READ | PROT_WRITE,
			   MAP_SHARED | MAP_POPULATE, fd, 0);
pg->magic = RFT_MAGIC;
pg->version = RFT_VERSION;
pg->nr_tasks = 1;
struct rft_task *me = &pg->task[0];
me->cpu = 4;
me->period_ns = 1000000;

/* 实时循环中，每帧结束时 */
struct rft_frame f = {
	.seq = k, .t_start = tk, .t_wake = r->wake,
	.t_end = now_ns(), .deadline = tk + period,
	.flags = overrun ? RFT_F_OVERRUN : 0,
};
rft_publish(me, &f);
```

rt-health 以只读方式映射该文件，用顺序号前后两次读取的方法检测读到一半的记录，不会向共享页写入任何内容。

`scripts/mock_rt.c` 是一个不依赖 EVL 的模拟生产者，每 100 帧故意超时一次，用于测试监测程序。

## 5. 验证情况

### 5.1 告警规则

| 级别 | 条件 | 是否需要基线 |
|---|---|---|
| FAIL | 实时任务通过帧遥测报告超周期 | 否 |
| WARN | 实时核上出现设备类中断；IPI 超过 5000 次/秒 | 否，属于配置错误，第一个采样即告警 |
| WARN | LLC MPKI、dTLB MPKI、IPC、后端停顿偏离本核基线 3.5 个稳健标准差以上 | 是，需约 15 个忙碌采样 |
| WARN | 温度事件中断超过 3000 次/秒 | 是 |

中断按来源分类计数。EVL 带外 LAPIC 定时器（SIPIC）和 Linux 的 proxy tick（SIRQC）是实时核的正常流量：运行 1 ms 周期任务时每核约 2000 次/秒，不计入告警。温度事件中断（TRM）不可迁移，本机空闲时每核也有约 800 次/秒，只在明显异常时报告。

基线只用“忙碌”的采样（利用率 > 3%）建立，统计类检查也只对忙碌的采样进行。空闲的核在一个采样周期内只执行几百条指令，其 MPKI 和 IPC 没有意义。

### 5.2 实机测试结果

平台：ThinkBook 16 G7+ IAH（Core Ultra 9 285H），EVL 内核 6.18.29，实时核 CPU 4、5，以 root 运行，监测进程绑定 CPU 0。测试期间非实时核上一直运行着一个 llama-server 进程（负载约 12）。

| 测试 | 方法 | 结果 |
|---|---|---|
| 单元测试 `make check` | 用本机真实格式的 `/proc/interrupts` 构造两次快照 | 定时器、温度、IPI、设备四类计数全部正确；设备名解析正确 |
| 无干扰时不误报 | evl-pingpong 60 s，同时在非实时核上运行 stress-ng（cache ×10，vm ×4 2 GiB） | 114 个采样全部 OK；pingpong 判定 PASS |
| 设备中断检测 | 把 Wi-Fi 中断 IRQ 183 的亲和改到 CPU 4，并 ping 网关产生流量 | 改动后的第一个采样即 WARN：`device IRQ on an RT core: 91/s, busiest IRQ 183: iwlwifi:default_queue`；恢复后回到 OK |
| 缓存劣化检测 | `tests/phases.c` 在 CPU 4 上先运行 50 s 缓存友好负载（64 KiB 顺序读），再运行 20 s 随机指针追逐（512 MiB），两阶段利用率相同（约 48%） | 平稳阶段 92 个采样全部 OK（IPC 4.84，LLC MPKI 0.003）；劣化阶段 36 个采样中 32 个 WARN（IPC 0.027，LLC MPKI 约 83，dTLB MPKI 约 79），其余 4 个位于阶段切换处 |
| 帧遥测 | `scripts/mock_rt.c` 每 100 帧故意超时一次 | 每 500 ms 报告 5 次超周期，状态 FAIL，并给出最坏帧耗时 |

### 5.3 监测本身的开销

evl-pingpong 在 CPU 4、5 上运行 60 s，分别在不运行监测、监测间隔 1000 ms、监测间隔 100 ms 三种条件下各测两轮（单位 µs）：

| 条件 | 唤醒延迟 平均 | 唤醒延迟 P99.9 | 唤醒延迟 最大 | 往返 最大 | 判定 |
|---|---:|---:|---:|---:|---|
| 无监测（两轮） | 1.72 / 2.06 | 6.40 / 6.24 | 16.1 / 16.9 | 55.6 / 55.2 | PASS |
| 1000 ms（两轮） | 1.71 / 2.31 | 6.04 / 4.51 | 18.4 / 11.6 | 44.4 / 54.3 | PASS |
| 100 ms（两轮） | 2.04 / 2.00 | 8.65 / 5.64 | 17.9 / 14.2 | 55.7 / 54.9 | PASS |

三种条件之间的差异与同一条件两轮之间的差异处于同一量级，在 60 s 测试的分辨率内未观察到监测带来的影响。更小的影响需要更长时间的测试才能分辨。

### 5.4 尚未验证

- 后端停顿（`stall%`）：本机 Intel PMU 不提供通用的 `stalled-cycles-backend` 事件，该列为空；需要改用 Intel 专用事件（如 `CYCLE_ACTIVITY.STALLS_MEM_ANY`）；
- 远端 NUMA 内存：本机只有一个 NUMA 节点，无法测试；
- 真实 EVL 任务接入帧遥测：目前只用模拟生产者验证，evl-pingpong 尚未调用 `rft_publish()`；
- 长时间运行下基线的漂移：基线窗口为 120 个忙碌采样，持续的劣化会逐渐被吸收进基线，告警随之消失。这是有意的设计（适应负载的正常变化），但持续劣化应由帧遥测的 FAIL 兜底。

## 6. 文件

```
src/main.c        主循环、参数、输出
src/perf.c/.h     perf_event_open：计数组、软件事件回退、停止后读取
src/sysfs.c       /proc/stat、/proc/interrupts、cpufreq、cpuidle、温度
src/metrics.c/.h  MPKI、IPC 等比率，中位数/MAD 基线，告警规则
src/telemetry.c/.h  帧遥测的只读映射
telemetry/rt_frames.h  实时任务端的发布接口（头文件，无依赖）
scripts/mock_rt.c 模拟实时任务
scripts/selftest.sh  冒烟测试（无需 root、无需 PMU）
scripts/ontarget/ 实机测试脚本：regress.sh（误报与中断注入）、cache-phases.sh（缓存劣化）、overhead.sh（监测开销）、graphic-demo.sh（图形界面演示）
tests/test_irq.c  中断分类单元测试（make check）
tests/phases.c    缓存行为分阶段变化的负载
tests/ptyshot.py  在伪终端中运行 -graphic 并保存最后一屏（无人值守测试用）
src/ui.c/.h       -graphic 全屏界面（ANSI 转义序列，不依赖 curses）
tools/irqcheck.py 列出一段时间内落到指定核上的中断
```
