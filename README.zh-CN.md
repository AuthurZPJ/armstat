# armstat

<p align="center">
  <a href="README.md">English</a> |
  <b>简体中文</b>
</p>

`armstat` 是一个面向 ARM 服务器的性能监控工具，风格上接近
`turbostat`，重点观察以下区间统计：

- CPU 频率
- Busy/Idle 时间
- package 功耗与能量
- NUMA / die 温度
- PMU 计数器与 IPC
- 拓扑元数据（package/core/NUMA）

它依赖 ARM 平台常见的 `sysfs`、`/proc/stat`、`hwmon`、
`thermal_zone` 和 `perf_event_open()`，而不是 x86 上统一的
MSR/RAPL/TSC 模型。

**关键差异点**：armstat 产出机器可读的 JSON 和 CSV 导出，带每样本
时间戳和稳定的 `schema_version`（当前为 4）。内置画图脚本，可以从
`armstat -f json -O data.json` 直接生成时序图表，无需手动数据处理。
详见 [EXPORTS.zh-CN.md](EXPORTS.zh-CN.md) 和
[PLOTTING.zh-CN.md](PLOTTING.zh-CN.md)。

## 文档导航

- **[DESIGN.zh-CN.md](DESIGN.zh-CN.md)** - 架构与实现细节
- **[TESTING.zh-CN.md](TESTING.zh-CN.md)** - 测试流程与验证方法
- **[EXPORTS.zh-CN.md](EXPORTS.zh-CN.md)** - JSON/CSV 导出格式规范
- **[PLOTTING.zh-CN.md](PLOTTING.zh-CN.md)** - 画图脚本使用说明
- **[CLAUDE.md](CLAUDE.md)** - AI 助手指引（英文）
- **[QWEN.md](QWEN.md)** - 项目上下文与技术概览（英文）

## 当前输出模型

`armstat` 当前使用 `SUM + CPU 行` 模型：

- 默认 text 模式每个 tracked CPU 只输出一行（不打印汇总或 package 行）
- `-a` 打开所有支持的基础列组，并在每核 CPU 行之上附加 package 聚合行和 `SUM` 汇总行
- `-S` 每个 interval 只输出一行 `SUM`
- JSON 输出 interval 对象数组
- CSV 输出 CPU 行或 summary 行

它是“ARM 上的 turbostat 风格工具”，但不是 x86 `turbostat` 的
逐列等价实现。

## 理解一轮输出的心智模型

理解 `armstat` 最简单的方式是把它看成“区间统计器”：

1. 先建立一份 baseline
2. 等待一个完整 interval
3. 再采一份新快照
4. 对两次快照做区间 delta / 百分比计算
5. 最后格式化成 `SUM`、CPU 行，或两者组合

因此，第一条可见输出永远代表“一个完整区间”，而不是程序启动瞬间的状态。

如果运行中发生 CPU 拓扑变化并触发 runtime state 重建，那么这次重建样本会被
当作新的 baseline，不会再被打印成一条普通 interval 输出。

理解输出时可以把它拆成三类问题：

- `Busy%` / `Idle%`：这个 interval 里看起来有多少时间忙 / 闲
- `LPI-*`：当 CPU 看起来在 idle 时，这部分 idle 又是如何分布到各个状态
- `SUM`：tracked CPU 的平均 / 聚合视图，不是某个“特殊 CPU”

## 关键语义

### CPU 编号

- 对外展示始终使用真实 Linux CPU ID
- 内部数组使用连续的 tracked index
- 输出时按真实 CPU ID 升序排序
- `--cpu` 过滤接受真实 CPU ID 和区间，例如 `0,1,4-7`
- `--cpu` 是采样过滤器，不只是输出过滤器：只有匹配列表的在线 CPU 才成为 tracked CPU，cpufreq、cpuidle、PMU、per-CPU Busy/Idle 输入以及 tracked-CPU 均值都基于该过滤集
- 非法 token、反向区间、匹配不到任何在线 CPU 的过滤器都是启动错误

### Idle / Busy

- `Idle%` / `Busy%` 由 busy-source 策略决定
- 默认策略是 `auto`：
  - 普通 CPU 使用 `/proc/stat`
  - 出现在 `/sys/devices/system/cpu/nohz_full` 里的 CPU 若支持，会优先使用
    `/proc/schedstat` 的运行时间记账
- `--busy-source procstat` 会强制所有 CPU 使用 `/proc/stat`
- `--busy-source schedstat` 会在可用时使用 `/proc/schedstat` 的 per-CPU
  运行时间记账；某个 CPU 不可用时会按 CPU 粒度回退到 `/proc/stat`
- `--busy-source task-clock` 保留为兼容别名，当前等价于 `schedstat`
- `IOWait%` 也来自 `/proc/stat`，表示本采样区间内处于 iowait 记账的
  时间占比
- 分 idle state 驻留列和唤醒列使用 cpuidle `stateN/name`，例如 `LPI-0`、
  `LPI-1`……以及 `LPI-0_wake`（每秒唤醒次数）
- cpuidle 只用于拆分 `LPI-*` 驻留，不作为 Busy/Idle 的权威来源
- 当没有 cpuidle 数据时，分 state 列会自动隐藏
- formatter 最多暴露八列 `LPI-*`（`LPI-0` ... `LPI-7`）；更深的 cpuidle
  state 会被折叠到最深可见的可用 residual bucket
- `Busy%` 的计算方式是 `100 - Idle%`
- 可见的 `LPI-*` 列是面向展示的语义，不是简单把 cpuidle 原始百分比直接
  打出来：
  - 较浅层、且可用的 state 保留原始 cpuidle 驻留百分比
  - 最深的“可用” state 会作为 residual bucket，用来补齐剩余 idle，
    使可见 `LPI-*` 之和接近该 CPU 的 `Idle%`
  - 如果最深层 state 被 disable，residual 会上移到最后一个仍可用的 state
- 因此，最深可见的 `LPI-*` 值可能是“补齐后的剩余 idle”，而不一定等于该
  state 的原始 `stateN/time` 百分比

### `SUM` 的含义

`SUM` 不是某个“虚拟 CPU”，而是 summary-scope 视角：

- 频率、功耗、能量、membw、PMU、系统计数等字段按 summary 规则聚合
- `Idle%`、`Busy%`、`IOWait%` 是 summary 级百分比
- summary 的 `LPI-*` 列是“各 CPU 最终显示值”的平均，而不是再次单独做一套
  全机 residual 计算

因此在阅读 `SUM` 时：

- 计数类字段更接近全机 interval 总量 / 聚合量
- 百分比类字段更接近 tracked CPU 的平均视角
- 当启用 `--cpu` 过滤时，默认不会再自动混出 `SUM`，避免把过滤后的 CPU 行和
  全系统 summary 混在一起
- 当启用 `--cpu` 过滤时，per-package 聚合行也会被抑制，因为它们会聚合
  过滤器之外的 CPU
- 使用 `--cpu` 时，tracked-CPU-derived 的 SUM 字段（如 frequency、idle、LPI、PMU）基于过滤后的 tracked CPU 集；平台/全局字段保持其自然 summary scope

### nohz_full 与 Busy/Idle

`nohz_full` CPU 在短 interval 下更容易让 `/proc/stat` 的 Busy/Idle
看起来抖动。因此默认的 `auto` busy-source 会在
`/sys/devices/system/cpu/nohz_full` 指定的 CPU 上优先使用
`/proc/schedstat` 的运行时间记账，而在其它 CPU 上继续沿用 `/proc/stat`。

### 功耗与温度

当前实现优先适配如下平台模型：

- package 功耗来自 `hwmon` 中 `name=power_meter` 的 `power1_average`
- summary 温度来自 `thermal_zoneN/temp`，在当前 `thermal-zone-index`
  策略下，其中 `N` 对应 NUMA/Vdie `N`

因此：

- `Power` 是 `SUM` 级字段，单位为 mW
- CPU 行的 `Temp` 来自该 CPU 所属 NUMA 节点温度，单位为 C
- `Temp0` ... `Temp3` 只在发现对应 NUMA/Vdie zone 后显示，单位为 C
- 目前不暴露 per-core power / per-core temperature

summary 温度策略现在是显式的，而不是隐含假设：

- 默认策略：`thermal-zone-index`
- 行为：`thermal_zoneN/temp -> TempN -> NUMA/Vdie N`
- 覆盖方式：`ARMSTAT_TEMP_POLICY=none` 可禁用 summary `TempN` 发现

`--probe` 会打印当前生效的 `summary_temp_policy`，方便排查平台差异。

### PMU

PMU 通过 `perf_event_open()` 实现：

- 每个 tracked CPU 单独建一组 perf counters
- 同一 CPU 上的事件按 perf group 打开
- group read 包含 `time_enabled` / `time_running`
- 多路复用事件会在导出 interval 值前做 scaling
- `-I` 会启用 `cycles,instructions` 并打开 IPC 列
- `-p ...` 只启用 PMU 计数器，不自动显示 IPC
- 未知 PMU 事件名和超过 `MAX_PMU_EVENTS` 的事件列表立即失败；perf 权限/打开失败仍降级为不可用值
- PMU 文件描述符使用量与 tracked CPU 数量成正比，`--cpu` 是降低 fd 压力的主要手段

PMU 一般需要 root 权限，或者较宽松的
`/proc/sys/kernel/perf_event_paranoid`。

如果显式请求了 PMU 或 IPC 列，但没有通过 `-p` 指定事件列表，
armstat 会默认使用 `cycles,instructions`。

## 架构

当前实现按职责拆为以下层次：

```text
armstat.c              主循环与模块生命周期
armstat_cli.c          命令行解析与列选择
collector.c            采样编排
sample_cache.c         内存池与快路径采样
idle_backend.c         busy-source 策略辅助（/proc/stat 与 /proc/schedstat）
aggregator.c           区间统计与 delta 计算
formatter_record.c     interval_record 构建与字段表
formatter_text.c       文本输出
formatter_machine.c    JSON/CSV 输出
cpu_inventory.c        present/online/tracked CPU 单一事实源
topology.c             package/core/NUMA 元数据
power_sensor.c         平台功耗/温度发现
power_interval.c       区间平均功耗与能量
membw.c                内存带宽计数
pmu.c                  基于 perf 的 PMU 采样
cpufreq.c              CPU 频率与 governor
cpuidle.c              cpuidle 状态驻留（LPI-*）
sysstat.c              /proc/stat 与 /proc/schedstat 读取
```

## 优化策略

当前实现的优化目标是“在大核数 ARM 服务器上把监控器自身的扰动压低”，
而不是每轮无差别读取所有数据源。

### 1. 三层采样模型

- **Static / rebuild 层**：
  CPU inventory、topology、传感器发现、cpuidle state 名称、PMU 事件元数据
- **Slow-changing 层**：
  CPU min/max 频率、governor、boost、传感器 capability、cpuidle `disable`
- **Per-interval fast path**：
  当前频率、`/proc/stat` delta、package 功耗、NUMA 温度、PMU 计数、
  cpuidle `stateN/time`

这样“平台上有什么”这类慢变化工作就不会落到每轮热路径里。

### 1.5. Busy/Idle 执行路径

当前 Busy/Idle 路径是显式的：

1. `sample_cache.c` 每轮采集一次原始累计计数器
2. 选定的 busy-source 策略决定每个 CPU 用哪一组原始计数器作为权威：
   - `/proc/stat idle/iowait`
   - 或 `/proc/schedstat` runtime（选定 CPU 上）
3. `aggregator.c` 将累计计数器换算为区间 delta
4. `Idle%`、`Busy%`、`IOWait%` 均从该 delta 导出

这一设计意味着 armstat 不再维护一个带私有“上一次样本”状态的隐藏
idle backend 对象。Busy/Idle 与 PMU、功耗、系统计数器共享同一显式
delta 时间线。

### 2. 预算式 slow refresh

slow layer 不是定时整批全量刷新，而是带游标、按预算渐进刷新。这样在大核
机器上不会出现周期性 CPU 占用尖峰。

具体实现是：

- 首次初始化或 hotplug 重建后，会先做一次全量刷新，建立完整 baseline
- 之后每个 interval 不再全机重扫，而是只计算一个“本轮最多刷新多少个 CPU”
  的预算
- 这个预算由采样间隔和目标 sweep 窗口（`SLOW_TARGET_SWEEP_MS`，当前约
  5 秒）决定，近似是：
  `budget ~= tracked_cpus * interval_us / target_sweep_us`
- 算出来的 budget 还会被夹在
  `SLOW_MIN_CPU_BUDGET .. SLOW_MAX_CPU_BUDGET` 之间，避免：
  - interval 很短时完全不刷新
  - 大核机器上单轮刷新过多
- `slow_cursor` 记录上一次停在哪个 tracked CPU
- 本轮只刷新 `[slow_cursor, slow_cursor + budget)` 这段 CPU，然后推进游标，
  到末尾后再回卷
- cpuidle 的 `disable` 缓存也复用同样的预算式刷新，所以运行中修改
  `stateN/disable` 会逐步反映出来，而不是触发一次全机 metadata 重扫
- slow layer 总是在 fast-path 快照之后执行，这样 interval 关键数据会先采，
  低频 housekeeping 不会拉长采样关键路径

所以 slow 数据最终仍会覆盖整机，但开销是被均匀摊到许多 interval 中的，
而不是每隔几秒突然冒出一次 CPU 尖峰。

### 3. 内核接口缓存

- `cpufreq` 当前频率读取使用缓存 FD
- `cpuidle stateN/time` 使用惰性 FD 缓存
- `/proc/stat` 使用缓存 `FILE *`，并且每个 interval 只解析一次

这样可以明显减少重复 `open()/close()` 和文本解析开销。

### 3.5. 尽力提升进程优先级

进入正常 interval 采样前，`armstat` 会尽力把自身 nice 调到更高优先级，
和 `turbostat` 的意图保持一致。如果内核拒绝该请求，程序仍会按当前优先级
继续运行，不会因此失败。

### 3.6. busy-source 与 nohz_full

默认的 busy-source 策略是 `auto`：

- 普通 CPU 使用 `/proc/stat`
- `nohz_full` CPU 在可用时优先使用 `/proc/schedstat`
- 如果某个 CPU 上 schedstat 不可用，则该 CPU 自动回退到 `/proc/stat`

`task-clock` 仍然保留为用户可见兼容选项，但当前等价于 `schedstat`，
因为 CPU 范围 perf `task-clock` 在目标 ARM 服务器上并不能可靠表达
Busy/Idle。

### 4. 按需采样

不是所有来源都会在每轮采样：

- 只有显示 `LPI-*` 时才刷新 cpuidle 分 state 数据
- 只有有可见字段依赖 package power 时才读取功耗
- 只有显示温度字段时才读取温度
- 只有 PMU active 时才读取 PMU

### 5. PMU grouping + scaling

PMU 以 tracked CPU 为单位建立 perf group。group read 会拿到
`time_enabled` / `time_running`，多路复用时先做 scaling，再导出 interval
delta。

### 6. 两阶段 formatter

输出分成两步：

1. `formatter_record.c` 构造稳定的 `interval_record`
2. `formatter_text.c` / `formatter_machine.c` 负责序列化

这样 text / JSON / CSV 共用同一套字段模型，不会在 serializer 里重复计算。

## 构建

```bash
make
```

armstat 可以从本仓库独立构建，也可以放入 Linux 源码树
`tools/power/armstat` 后用同样的 `make` 命令构建。交叉编译通过
`CROSS_COMPILE` 支持（例如 `CROSS_COMPILE=aarch64-linux-gnu-`），
外部构建通过 `make O=/path/to/output` 支持。

## 用法

### 基本用法

```bash
armstat
armstat -i 5
armstat -n 10
armstat -D
armstat --busy-source auto
armstat --busy-source procstat
armstat --busy-source schedstat
armstat --busy-source task-clock
```

### 其他选项

- `-N, --header-iterations N` — 每 N 个 interval 重印一次 text 表头
- `-J, --joules` — 显示区间能量（焦耳）
- `-q, --quiet` — 抑制 interval banner 和 text 表头
- `-v, --version` — 显示版本并退出

### 输出格式

```bash
armstat -f text
armstat -f json
armstat -f csv
armstat -f json -O armstat.json
armstat -f csv -O armstat.csv
```

`-O` / `--export` 是 `-o` / `--output` 的导出别名。它尤其适合
JSON / CSV 这类机器可读输出，但 text 模式也同样可用。

CSV 导出现在会在每行前面附带 `schema_version`、`interval`、`timestamp`、
`timestamp_iso` 四列，方便后处理脚本直接按真实时间对齐样本，并识别当前
导出契约版本。

更完整的 JSON/CSV 字段与结构说明已经单独整理到 [EXPORTS.zh-CN.md](EXPORTS.zh-CN.md)
（中文）和 [EXPORTS.md](EXPORTS.md)（英文）。

### 摘要模式

```bash
armstat -S
armstat -S -a
```

`-S` 表示只输出摘要行，`-a` 表示打开所有支持的基础列组，并且不会
隐式启用 PMU/IPC。
在 text/JSON 模式下，如果使用 `-a`，或者通过 `-s` 显式选择了
summary 级列组，那么启用了 system 级字段时会额外打印 `SUM` 区域。
package 聚合行只有在同时启用 package 列组（`-s pkg` 或 `-a`）时才会出现。

当启用了 `--cpu` 过滤时，为避免“过滤后的 CPU 行”和“全系统 SUM”
混在一起，默认不会再自动附加这个 `SUM` 区域；如果确实要只看摘要，
请显式使用 `-S`。

### CPU 过滤

```bash
armstat -c 0,1,2-5
```

`--cpu` 接受真实 Linux CPU ID 和范围，在运行时采样开始前应用该列表。
过滤后的运行只追踪匹配的在线 CPU，因此 per-CPU 行、summary 均值、
PMU group、cpufreq 读取、cpuidle 刷新以及 per-CPU Busy/Idle 记账
都基于过滤后的 tracked 集合。纯平台/全局 summary 字段仍保持
summary scope。

解析器是严格的：非法 token（如 `bad`）、反向区间（如 `3-1`）、空
token（如 `0,,2`）以及匹配不到任何在线 CPU 的过滤器都会作为启动
错误报出。

### 列控制

```bash
armstat -s all
armstat -s power,temp
armstat -s pkg,core,node,freq,idle
armstat -s LPI-0,LPI-1,Idle%,Busy%
armstat -H temp
```

`-s` 与 `-H` 均会对未知列组或字段名报启动错误。

支持的列组别名：

- `cpu`
- `pkg`, `package`——per-package/socket 聚合行
- `core`
- `numa`, `node`
- `freq`
- `idle`
- `power`
- `temp`
- `pmu`
- `sysstat`, `irq`
- `membw`, `mem`
- `ipc`
- `energy`, `joules`
- `all`

`-s` / `-H` 现在也支持精确字段名，例如 `Idle%`、`Busy%`、`IOWait%`、
`SoftIRQs`、`LPI-0`、`Power`。

### PMU

```bash
armstat -p cycles,instructions
armstat -p cache-misses,branches
armstat -I
```

内置 PMU 事件名：

- `cycles`
- `instructions`
- `cache-references`
- `cache-misses`
- `branches`
- `branch-misses`
- `mem-access`
- `mem-read`
- `mem-write`
- `l1d-cache-refill`
- `l1d-cache`
- `l1i-cache-refill`
- `l1i-cache`
- `l2d-cache-refill`
- `l2d-cache`
- `l3d-cache-refill`
- `l3d-cache`

原始 ARM PMU 事件配置也可以用十六进制值指定，例如 `0x11`。未知事件名和超过
`MAX_PMU_EVENTS` 的列表在采样开始前失败。如果事件已知但当前机器上 perf
不可用，请求的 PMU 列保留可见并渲染为不可用，而非报告假零。

### 辅助输出

```bash
armstat -l
armstat --probe
```

`-l` 当前会打印内置列组和 PMU 事件名。
`--probe` 会一次性打印当前平台的能力摘要，包括 CPU 拓扑、
effective busy-source 策略、cpuidle/LPI 可用性、温度源、
内存带宽支持情况，以及基础 PMU 可用性探测。

### 画图

附带的画图脚本说明已经单独整理到 [PLOTTING.zh-CN.md](PLOTTING.zh-CN.md)
（中文）和 [PLOTTING.md](PLOTTING.md)（英文）。

### 导出契约

机器可读导出的字段和结构说明已经单独整理到 [EXPORTS.zh-CN.md](EXPORTS.zh-CN.md)
（中文）和 [EXPORTS.md](EXPORTS.md)（英文）。

### 测试

测试说明已经单独整理到 [TESTING.zh-CN.md](TESTING.zh-CN.md)（中文）和 [TESTING.md](TESTING.md)
（英文）。

## 字段与作用域

当前字段模型明确区分 summary 级和 CPU 级。

### Summary 级字段

- `AvgFreq`
- `UncoreFreq`
- 分 idle state 驻留列（使用 cpuidle `stateN/name`，例如 `LPI-*`）
- `Idle%`
- `IOWait%`
- `Busy%`
- `Power`
- `Temp0` ... `Temp3`
- `Energy`
- `MemBW`
- `CtxSw`
- `IRQs`
- `SoftIRQs`
- `IPC`
- 聚合后的 PMU 计数

### CPU 级字段

- `CPU`
- `Pkg`
- `Core`
- `Node`
- `Freq`
- `Min`
- `Max`
- `Governor`
- `Boost`
- 分 idle state 驻留列（使用 cpuidle `stateN/name`，例如 `LPI-*`）
- `Idle%`
- `IOWait%`
- `Busy%`
- `IPC`（启用时）
- `Temp`，来自 NUMA / die 温度映射
- 每 CPU PMU 计数

对于 idle 相关字段，需要特别注意：

- `Idle%` / `Busy%` 以 busy-source 选中的权威来源为准
- 可见 `LPI-*` 之和会尽量贴近 `Idle%`
- 因此最深可见的 `LPI-*` 列可能是经过 residual 调整后的展示值，而不是
  cpuidle 原始值直出

## 数据来源与计算方法

这一节描述的是当前代码里的真实实现，包括有意做过的展示层调整。

### CPU 身份与拓扑

- **CPU / Pkg / Core / Node**
  - 来源：
    `cpu_inventory.c` 与 `topology.c`
  - 语义：
    真实 Linux CPU ID，加上 package/core/NUMA 元数据
  - 备注：
    输出按真实 CPU ID 排序，内部数组仍使用 tracked index

### 频率相关字段

- **Freq**
  - 来源：
    `/sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq`
  - 单位：
    MHz
  - 公式：
    `Freq = scaling_cur_freq / 1000`

- **Min / Max**
  - 来源：
    `scaling_min_freq`、`scaling_max_freq`
  - 单位：
    MHz
  - 公式：
    `Min = scaling_min_freq / 1000`，`Max = scaling_max_freq / 1000`

- **Governor**
  - 来源：
    `scaling_governor`
  - 备注：
    属于 slow-changing 层字段

- **Boost**
  - 来源：
    优先 per-CPU `cpufreq/boost`，其次全局 `cpu/cpufreq/boost`
  - 值：
    `1`、`0`，不可用时显示 `-`

- **AvgFreq**
  - 每 CPU 公式：
    `(prev_cur_freq + cur_freq) / 2`
  - summary 公式：
    对所有有效 tracked CPU 的 per-CPU interval MHz 求平均
  - 备注：
    这是基于两个采样点的 interval-average 近似值，不是 APERF/MPERF 那类
    硬件平均频率

- **UncoreFreq** / `uncore_freq`
  - 作用域：
    仅 summary
  - 来源：
    设备名看起来像 uncore/interconnect/fabric，且唯一可判定的
    `/sys/class/devfreq/<device>/cur_freq`
  - 单位：
    MHz
  - 公式：
    `uncore_freq = cur_freq_hz / 1000000`
  - 说明：
    这是平台级 devfreq 读数，不是 per-CPU 字段；如果 devfreq 拓扑有歧义，
    armstat 会隐藏 `uncore_freq`，而不是猜一个看起来像对的设备

### Busy / idle 相关字段

- **Idle%**
  - 来源：
    busy-source 策略选中的权威来源
  - 角色：
    当前 CPU 在整个采样区间内的权威 idle 预算
  - 公式：
    - `procstat`：`/proc/stat` 的 per-CPU `idle`，再做
      `idle_delta_us / interval_delta_us * 100`
    - `schedstat`：先从 `/proc/schedstat` 的 per-CPU 运行时间得到
      `Busy%`，再由 `Idle% = 100 - Busy%`
    - `task-clock`：兼容别名，当前等价于 `schedstat`
  - 解读：
    `Idle%` 表示“这个采样窗口里，该 CPU 总体有多 idle”。armstat 会把
    它当作分 `LPI-*` 展示时要解释清楚的基准值。

- **IOWait%**（默认关闭；可通过 `-s iowait` 或 `-s IOWait%` 开启）
  - 来源：
    `/proc/stat` 的 per-CPU `iowait`
  - 公式：
    `iowait_delta_us / interval_delta_us * 100`

- **Busy%**
  - 来源：
    派生值
  - 公式：
    `100 - Idle%`
  - 备注：
    当前 `IOWait%` 会单独显示，但不会再从 `Busy%` 中额外扣除
  - 解读：
    当前模型满足 `Idle% + Busy% = 100`；`IOWait%` 是额外的参考拆分，
    不是第三个互斥分桶

### LPI / cpuidle 相关字段

- **LPI-* 列名**
  - 来源：
    `cpuidle/stateN/name`

- **原始驻留输入**
  - 来源：
    `cpuidle/stateN/time`
  - 公式：
    `state_delta_us / interval_delta_us * 100`
  - 含义：
    cpuidle 对“本采样区间里每个 idle state 驻留了多久”的原始视角

- **Idle% 和原始 LPI 的关系**
  - `Idle%` 回答的是：“这个 CPU 整体有多 idle？”
  - 原始 `LPI-*` 回答的是：“cpuidle 把 idle 时间记到了哪些 state 上？”
  - 在一些 ARM 平台上，短 interval 的 cpuidle state 记账可能会滞后于
    busy-source 给出的整体 idle 视角
  - 因此原始 `sum(LPI-*)` 不一定天然等于权威的 `Idle%`

- **实际显示语义**
  - 较浅层可用 state 尽量保留 cpuidle 原始百分比
  - 最深可见、且可用的 state 作为 residual bucket：
    `Idle% - sum(较浅层可见 LPI)`
  - 如果最深层 state 被 disable，则 residual 上移到最后一个仍可用的 state
  - 目的：
    让可见 `LPI-*` 之和尽量贴近权威的 `Idle%`
  - 结果：
    最终显示出来的 `LPI-*` 是面向解释 `Idle%` 的展示值，并不总是
    `cpuidle/stateN/time` 原始百分比的直接照抄

### 功耗与能量

- **Power**
  - 来源：
    package 级 `power_meter/power1_average`
  - 单位：
    mW
  - 备注：
    当前实现将 package 功耗映射到 `SUM` 行，不在 CPU 行伪造 per-core 功耗

- **区间平均功耗**
  - 公式：
    `(prev_power_mw + cur_power_mw) / 2`

- **Energy**
  - 公式：
    `interval_avg_power_mw * interval_seconds / 1000`
  - 备注：
    当前显示的是区间能量，不是累计能量

### 温度

- **Temp0 ... Temp3**
  - 来源：
    `thermal_zoneN/temp`
  - 映射：
    `thermal_zoneN -> TempN -> NUMA/Vdie N`
  - 单位：
    摄氏度

- **CPU 行 Temp**
  - 公式：
    取该 CPU 所属 NUMA 节点的温度
  - 备注：
    这是 NUMA 级 summary 温度映射，不是 per-core sensor

### 系统计数

- **CtxSw**
  - 来源：
    `/proc/stat` 的 `ctxt`
  - 公式：
    `ctxt_now - ctxt_prev`

- **IRQs**
  - 来源：
    `/proc/stat` 的 `intr`
  - 公式：
    `intr_now - intr_prev`

- **SoftIRQs**
  - 来源：
    `/proc/stat` 的 `softirq`
  - 公式：
    `softirq_now - softirq_prev`

这三者当前都是“本 interval 发生了多少次”，不是除以时间后的速率。

### 内存带宽

- **MemBW**
  - 来源：
    平台相关的原始内存带宽字节计数器
  - 公式：
    `(counter_now - counter_prev) / interval_seconds`
  - 单位：
    MB/s
  - 备注：
    如果平台没有可用的 raw counter，`MemBW` 可能长期为 0 或不可用

### PMU 与 IPC

- **PMU 事件列**
  - 来源：
    每个 tracked CPU 上的 `perf_event_open()`
  - 模型：
    per-CPU perf group，读取时带 `time_enabled / time_running`
  - 显示值：
    对 scaling 后的累计计数做 interval delta

- **Summary PMU**
  - 公式：
    对 tracked CPU 的 per-CPU scaled PMU 计数求和，再导出 interval delta

- **IPC**
  - 公式：
    `instructions / cycles`
  - 作用域：
    summary 和 CPU 两种视角都支持

## 降级与失败语义

`armstat` 的设计目标之一是：某个来源不可用时，尽量让其它能力继续工作。

- 如果 `cpuidle` 不可用或被关闭：
  - `Idle%` / `Busy%` 仍然正常
  - `LPI-*` 列隐藏
- 如果 PMU 打不开：
  - 显式请求的 PMU / IPC 列仍然保留
  - 值显示为不可用，而不是伪装成 `0`
- 如果 package 功耗或 NUMA 温度源不可用：
  - 对应字段显示不可用或隐藏
  - 不影响其它字段
- 如果运行中发生 CPU hotplug：
  - inventory、sample cache、cpuidle 运行态、PMU、topology 会一起重建
  - 下一条样本会作为新的 baseline，避免跨 hotplug 边界混算 delta
- 如果 `nohz_full` 让短 interval 的 `/proc/stat` 抖动：
  - 默认 `auto` 会优先对这些 CPU 使用 `/proc/schedstat`
  - 拉长 interval 往往仍然更容易解释

## 平台说明

当前传感器策略假定：

- package 功耗来自 `power_meter/power1_average`
- summary 温度遵循显式的 `thermal-zone-index` 策略：
  `thermal_zoneN/temp -> TempN -> NUMA/Vdie N`

例如：

- 1 路 / 2 NUMA 机器通常显示 `Temp0` 和 `Temp1`
- 2 路 / 4 NUMA 机器通常显示 `Temp0` 到 `Temp3`

如果你的平台暴露的是另一套传感器布局，可以先通过
`ARMSTAT_TEMP_POLICY=none` 关闭 summary `TempN` 发现，或者相应调整
`power_sensor.c` 中的 summary 温度策略。

## 当前限制

- 输出模型仍是 `SUM + CPU 行`，还没有像成熟 `turbostat` 那样的独立
  core 聚合行（per-package 聚合已实现）
- per-core power 尚未实现
- CPU 行温度是 NUMA/die 温度映射，不是 per-core 传感器
- PMU scaling 已接入，但仍需要在真实目标机上继续验证
- 没有足够权限时，PMU 初始化会失败

## 调试

可用 debug 模式查看初始化与运行时路径：

```bash
armstat -d -i 1 -n 2
```

建议重点确认：

- 当前 effective busy-source 是什么，以及 cpuidle/LPI 是否可用
- tracked CPU inventory 是否正确
- PMU 是否初始化成功
- 功耗和温度传感器是否按预期发现

## 相关工具

- [turbostat(8)](https://man7.org/linux/man-pages/man8/turbostat.8.html)
- [perf(1)](https://man7.org/linux/man-pages/man1/perf.1.html)

## 许可证

GPL-2.0
