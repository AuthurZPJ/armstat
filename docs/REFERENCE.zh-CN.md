<!-- SPDX-License-Identifier: GPL-2.0 -->
# armstat 综合参考

<p align="center">
  <a href="../README.zh-CN.md">README</a> | <a href="REFERENCE.md">English</a>
</p>

本文集中说明实现、指标公式、集成和验证。[README](../README.zh-CN.md) 负责日常
使用，安装后可用 `man armstat` 查询命令选项。

## 仓库布局

| 目录 | 职责 |
|---|---|
| `src/app/` | 命令行与进程生命周期 |
| `src/core/` | 采集、聚合、CPU 清单、采样时刻管理 |
| `src/platform/` | Linux/ARM 数据源后端 |
| `src/output/` | 字段注册、独立记录、序列化 |
| `scripts/` | 导出加载器与绘图 |
| `tests/` | 回归测试与 ARM64 目标机验收 |
| `man/`、`docs/` | 安装后的手册与参考 |

头文件是对应实现旁的内部接口，项目不提供公开 C 库 API。

## 运行架构

```text
采集 sys_snapshot -> 计算 interval_stats -> 构造 interval_record -> 序列化
```

| 模块 | 负责的工作 |
|---|---|
| `app/armstat_cli.c`、`app/armstat.c` | 参数、启动、采样循环、信号、输出收尾与清理 |
| `app/command.c` | 命令进程组、启动门、终端访问、信号转发与等待状态 |
| `core/collector.c` | 编排原始快照，提供共享读取接口 |
| `core/sample_cache.c` | 复用缓冲区、缓存读取、慢速刷新 |
| `core/aggregator.c` | 区间增量、有效性、CPU/插槽聚合、提交下一轮基线 |
| `output/columns.c` | 字段描述与可见性 |
| `output/formatter_record.c`、`formatter_values.c` | 保存记录自身的数据，提供按类型读取字段的接口 |
| `output/formatter_text.c`、`formatter_json.c`、`formatter_csv.c` | 序列化；机器输出共用辅助函数在 `formatter_machine.c` |

采集阶段读取原始样本，聚合阶段计算区间增量、百分比和速率。序列化器使用一份独立记录，
不持有指向可变快照数据的指针。
原始诊断在一次快照后直接使用 `output/formatter_raw.c`，不经过区间聚合或显示
字段筛选。

首份快照建立基线。后续采样时刻锚定单调时钟，采集与输出耗时从下一次等待中
扣除；错过整个周期时跳过该时刻，不突发追赶。公式始终使用实测 `interval_delta_us`。

命令模式（`armstat [选项] -- command [参数...]`）先完成初始化，再在放行子进程
执行前建立新基线。等待并回收命令后，结束快照形成一个区间，包含少量采集、启动
和等待开销。命令独立成进程组，INT/TERM/HUP/QUIT 转发给该组，并保留前台终端
访问及 shell 停止/继续行为。
初始化失败时不执行命令。CPU 热插拔重置窗口、采集失败或输出失败返回 1，不声称
得到有效样本。测量和输出成功时保留命令状态，包括非零退出：找不到命令为 127，
其他 exec 失败为 126，信号终止为 128 加信号编号。统计仍覆盖所选 CPU/平台，
不代表仅该进程的工作量。

### CPU 身份

`cpu_id` 是内核路径和输出中的真实 Linux 编号；`tracked_idx` 是所选在线 CPU
集合中的连续下标。`cpu_inventory.c` 管理存在、在线与所选 CPU 清单，内部使用
`for_each_tracked_cpu` 遍历。`--cpu` 过滤采集和由 CPU 数据计算的汇总值；全局
计数器与平台传感器保留自身范围。编译表示范围为 ID `0..1023`，更大的清单会报告截断。

热插拔检查读取紧凑的 online 掩码，仅在变化或读取失败后扫描目录，并用两次观察
消除短暂变化。重建会重置 CPU 相关缓存、拓扑、PMU 与聚合状态。重建样本作为
新基线，完成后重新采集时钟，避免重建耗时计入下一测量窗口。

### 三层采样

| 层次 | 数据 |
|---|---|
| 启动/重建 | CPU 成员、拓扑、传感器、空闲状态名称、PMU 元数据 |
| 慢速刷新 | 频率上下限、governor、boost、cpuidle disable 标志 |
| 每区间 | 所选频率、Busy/Idle、LPI、功耗、温度、带宽与 PMU 输入 |

慢速数据先全量刷新，之后在快速快照完成后按滚动 CPU 游标刷新。默认预算为
`ceil(所选 CPU 数 * 区间时长 / 5秒)`，限制在每轮 1–32 个 CPU；上限可能使
完整刷新超过五秒。因此运行中修改 disable 标志会逐步生效。

读取按字段和输出层级选择。只有 Busy/Idle/IOWait 或系统计数器需要时，才在该
区间解析一次 `/proc/stat`；仅选择 LPI 不需要它。cpuidle 只读选中的状态和计数器：
驻留统计不需要 usage，usage 也不需要 time。没有 LPI 需求时不初始化 cpuidle，
也不发出其可用性告警；`--probe` 的能力发现不受 `-s`/`-H` 限制。
cpufreq/cpuidle 的 FD 缓存与 procstat
流缓存减少重复打开和解析。不能输出的 CPU/插槽层级也不构造记录。采样前会尝试
降低自身 nice 值，失败不影响继续运行。

## 指标来源与计算

`armstat --list` 是精确字段 ID、层级、类型、单位、标签和 JSON key 的查询入口。
下表中 `Δ` 表示相邻有效样本的差值，`T_us` 是实测微秒时长，`T_s = T_us / 1000000`。

| 字段 | 来源 | 计算/单位 |
|---|---|---|
| `CPU`、`Pkg`、`Core`、`Node` | CPU 清单与 Linux 拓扑 | 真实编号；CPU 行按编号排序 |
| `Freq` | CPU `cpufreq/cpuinfo_cur_freq` | kHz / 1000 → MHz；当前采样值 |
| `Min`、`Max` | `scaling_min_freq`、`scaling_max_freq` | kHz / 1000 → MHz；配置上下限 |
| `Governor`、`Boost` | `scaling_governor`、每 CPU 或全局 `cpufreq/boost` | 缓存字符串/布尔值 |
| `UncoreFreq` | 唯一匹配的 `/sys/class/devfreq/<device>/cur_freq` | Hz / 1000000 → MHz；仅汇总 |
| `Idle%`（procstat） | 每 CPU `/proc/stat` idle + iowait | 将 `Δ` 从 USER_HZ tick 换成微秒，再 `/ T_us * 100` |
| `IOWait%` | 每 CPU `/proc/stat` iowait | 将 `Δ` 换成微秒，再 `/ T_us * 100` |
| `Busy%` | 利用率策略 | `100 - Idle%`；schedstat 使用运行时间 `Δ / elapsed_ns * 100` |
| `LPI-N`、`LPI-N_usage` | CPU `cpuidle/stateN/time`、`usage` | `Δtime_us / T_us * 100`；`Δusage / T_s` |
| `Power` | 唯一 `power_meter/power1_average` 传感器 | 两端功耗采样值的平均，mW；汇总字段 |
| `Energy` | 由功耗推导 | `Power_mW * T_s / 1000` → 区间 J |
| `Temp0` ... `Temp3`、CPU `Temp` | 下述温度策略 | 毫摄氏度 / 1000 → °C |
| `MemBW` | 唯一 `/sys/class/memory/mem*/mem_bytes_read` | `Δbytes / T_s / 1024²` → MiB/s |
| `CtxSw`、`IRQs`、`SoftIRQs` | `/proc/stat` ctxt、intr、softirq | `Δ` → count/interval |
| PMU、`IPC` | 每 CPU perf 事件组 | 缩放后的区间计数；`instructions / cycles` |

SUM/插槽频率是当前有效 CPU 样本的平均，不是区间平均或忙碌频率。Energy 用
两端平均估算采样信号的区间能量，不是硬件能量计数器或累计值。系统和 PMU
计数不是每秒速率；显示精度也不代表传感器精度。

### Busy/Idle 与 LPI

Auto/procstat 对所有所选 CPU 使用 `/proc/stat`，包括 `nohz_full` CPU。
有效利用率在 0–100 之间，满足 `Idle% + Busy% = 100`。Procstat 的 Idle 包含
iowait，因此它已从 Busy 中排除。Schedstat 从任务运行时间推导 Idle；此模式下
IOWait 仍是独立 procstat 观察值，不保证是该 Idle 的子集。

显式 schedstat 用于诊断：运行时间在任务切换时结算，仍在执行的时间可能遗漏。
读取器接受版本 10–17，要求 CPU 行含九个字段，以第七字段作为运行时间；不支持
或不合法的记录按 CPU 回退到 procstat。`task-clock` 是同一路径的别名。
短 procstat 区间受 USER_HZ 精度影响；jiffy 增量无法安全换算为微秒时显示不可用。
内核记账路径见
[`sched_info_depart()` / `sched_info_switch()`](https://github.com/torvalds/linux/blob/bd5f485f3f026225b86573e559af0b7254ef4184/kernel/sched/stats.h)。

LPI 是各状态独立的 sysfs 统计，不分配残差、不截断到 100%，默认无 `IdleGap`
字段，其和不必等于 Idle。仅暴露状态 0–7，更深状态不折叠到其他状态；`--probe`
报告 `idle_state_N_name` 映射。

时间在状态退出时结算，长时间空闲可能在退出前显示 0、退出后超过 100%。软件
驻留统计不能证明硬件深度。状态/计数器缺失、禁用、读取失败或回退时独立显示
不可用。SUM 的每个 LPI 状态要求所有所选 CPU 都有有效值，再计算平均；缺失
CPU 不按零处理。

### PMU

PMU 采集与原始计数列显示分开控制。默认、`-S`、`-a`、`-s all` 和 `-s cpu`
快捷视图用 `cycles,instructions` 尝试提供 IPC，不增加原始计数列或 CPU 行。
所选 N 个 CPU 需要 2N 个 perf FD。任一 CPU 组启动失败时，关闭全部自动计数器
并隐藏 IPC，仅 `-d` 输出诊断。`-H ipc` 关闭自动采集，其他白名单只有选择
IPC 才采集它。
`-P`/`--no-perf` 禁止 PMU 初始化及 `--probe` 的 perf 测试；无论选项顺序，
与显式 PMU/IPC 请求组合都报参数错误。Busy/Idle 来源规则保持不变。

显式 `-I`、`-p` 或 IPC 字段/列组请求保留诊断与不可用列。`-I` 显示原始计数
及 IPC，单独选择 IPC 不显示原始计数。自定义 `-p` 列表抑制自动 IPC；明确
请求 IPC 时再补入 cycles/instructions。自动启动成功后，后续无效读取仍保留
不可用 IPC 列，PMU 有效性变化本身不改变 CSV 列。

每个所选 CPU 打开一个 perf 事件组。区间计数使用精确整数运算计算
`round(Δcount * Δtime_enabled / Δtime_running)`。首次、失败、短读、回退或
未运行的读取不可用。时间增量不合法、缩放或每 CPU 累计总量溢出时，整组失效
并重建累计基线，后续有效区间可恢复。

汇总累加每 CPU 的区间增量，要求全部所选 CPU 有效，并拒绝 64 位区间和溢出。
IPC 要求命名为 `cycles` 和 `instructions` 的有效计数且 cycles 非零；汇总 IPC
使用聚合计数计算。

最多选择 16 个不重复的事件名称。原始配置要求 `0x`/`0X` 后跟不超过 64 位的
十六进制数，不接受符号或空白。`mem-read` 与 `mem-write` 是退休的加载/存储
次数，不是字节数；可选缓存事件依赖具体 CPU。完整列表见 `--list`。被 perf
拒绝的显式事件仍显示为不可用。FD 用量随 CPU/事件数增长；初始化尝试在硬限制
内提高软 `RLIMIT_NOFILE`。显式请求在预算仍不足时告警；自动 IPC 除 debug 模式
外静默回退。

### 传感器映射与缺失数据

功耗和内存带宽要求恰好一个匹配来源；零个或多个候选都保持不可用。`--probe`
报告路径、候选数和歧义。Uncore devfreq 也要求唯一匹配来源。

`ARMSTAT_TEMP_POLICY=thermal-zone-index` 将 `thermal_zoneN/temp` 映射为
`TempN` 与 NUMA/裸片 N，CPU `Temp` 使用其 NUMA 节点温度。保留稀疏编号与负温度。
`none`、`disabled` 关闭汇总温度发现，其他值导致启动失败。需在服务器上确认映射。
不提供逐核功耗/温度或专门的核心聚合行。

不可用的区间输入在聚合和输出中继续保持不可用。读取失败只影响依赖字段；
累计来源恢复时重建基线，避免跨缺失窗口求差或输出伪零。有效零保持为数据。

## 输出契约

JSON/CSV 与随附绘图加载器使用 **schema 第 9 版**。第 8 版 LPI 是补齐后的值，
不能按独立状态统计解释或与第 9 版混用。`--list` 来自序列化器共用的字段注册表。
独立的 key-value 探测契约为 `probe_schema_version: 1`，PMU 探测仅覆盖首个 CPU 的 cycles。

### 字段选择

默认输出 SUM，`-a` 增加 CPU 动态指标和 Pkg/Core/Node 拓扑，省略插槽聚合行、
频率配置与 LPI usage。`-s all` 补入这些列组。这些内置视图包含可用 IPC；
原始 PMU 计数始终需要显式请求。
`MemBW` 需要显式 `-s membw`（或 `mem`）、`-s mem_bw`，可与其他选择组合，如
`-s all,membw`。默认、`-S`、`-a` 和 `-s all` 均不会自动启用它。
`freq` 选择当前频率/UncoreFreq，`freq-config` 选择 Min/Max/Governor/Boost，
`idle` 选择驻留与 Busy/Idle/IOWait，`idle-usage` 选择每 CPU 状态进入速率。

列组不区分大小写，优先于精确稳定 ID；稳定 ID 又优先于文本/JSON 别名。
重名别名选择字段但不展开行层级。稳定 CPU 字段 ID 及
cpu/core/numa/freq-config/idle-usage 列组启用 CPU 行，插槽字段 ID 与 pkg/package
启用插槽行。仅属 CPU 的 `Min`、`Governor`、`lpi0_usage` 等别名也启用 CPU 行。
单独 `-s cpu` 提供常用 CPU 动态指标且不带 SUM，保留显式隐藏请求；组合选择
按请求的字段与层级输出。独立 PMU/IPC/能量请求保留所需 SUM 输出。CPU 过滤后
展开 CPU 行仍保留所选 CPU 的 SUM；`-s cpu` 快捷视图保持仅 CPU，插槽行按选择
输出。全局计数器和平台传感器保留自身范围。首个 `-s` 替换之前的视图：
`-a -s cpu` 使用 CPU 快捷视图，`-s cpu -a` 使用扩展动态视图。
重复 `-s` 扩展白名单，包括 `-s all` 的完整选择，不会恢复被 `-H` 隐藏的无关
列组，包括此前显式请求的 PMU、IPC 或能量字段。
若初始化后所选视图只剩 CPU 行标识而没有数据列，启动报错。已显示列的采样值
不可用时，选择本身仍有效。

### 稳定区间元数据

| 字段 | 含义 |
|---|---|
| `schema_version` | 整数兼容性门槛 |
| `interval` | 从 1 开始的可见样本编号 |
| `duration_us` | 单调时钟实测窗口，微秒 |
| `timestamp` | 样本结束的 Unix 整秒时间 |
| `timestamp_ns` | 同一时刻的纳秒值；优先用于程序时间轴 |
| `timestamp_iso` | 同一结束时刻的本地 RFC 3339 时间，带九位小数 |

Duration 描述测量窗口，三个时间戳表示同一结束时刻。导出速率的分母应使用
实测时长，不使用请求的采样区间。

### 文本

仅 SUM 输出采用连续表格，每个区间一行带时间的数据，适用于默认、`-S` 和只选
汇总字段的 `-s`。其他视图保留区间标记、实测秒数和空行分隔。文本时间保留
日期/时区，含三位小数。表头含单位，标签、选择或宽度变化时重印；行标识宽度
适配稀疏 package ID。`-q`/`--quiet` 只隐藏启动说明；`--quiet-all` 还省略标记、
时间和表头，机器字段 key 不变。

文本输出到可取得有效高度和宽度的终端时，默认自动重复表头。仅 SUM 视图跨区间
累计屏幕行预算；其他视图在每个区间的各段起始打印表头，较长的 CPU/package 表在区间内
继续重印。每个区间读取终端尺寸以跟随窗口变化；按所选字段、PMU、行标识及仅
SUM 视图的时间列宽估算换行，并预留两行屏幕空间。若放不下一个表头加一条数据，
停止区间内及 SUM 滚动重印，仍保留启动、布局变化和各段起始的表头。
这只控制表头位置，不裁列、分页或重绘屏幕。显式 `-N N` 覆盖此策略：正数每 N
个样本重复，`-N 0` 仅在启动和布局变化
时打印。管道、重定向及无法取得有效尺寸的终端默认采用后者；`--quiet-all` 始终省略区间表头。

`-D`/`--Dump` 使用独立诊断文本格式，不睡眠，只收集一次后端可用的 proc/sysfs
来源，忽略视图/字段选择；`-c` 仍限定 CPU。正文包含累计 procstat/schedstat、
cpuidle 计数、CPU 配置、内存读取字节和当前传感器，并标明单位。
Dump 无需切换来源即可读取 schedstat，并包含最多 16 个 cpuidle 状态，超出常规
八列 LPI 的显示范围。不计算区间增量、速率、百分比或能量，也不打开 perf 事件。
quiet 选项不隐藏诊断正文，`-N` 无效。
Dump 与显式 `-i`/`-n`、命令、`--probe`、`-l`、非文本格式和显式 PMU/IPC
请求冲突；单次区间采样改用 `-n 1`。

### JSON

顶层数组每区间一个对象，含元数据及所选的 `summary`、`packages`、`cpus` 层级。
插槽使用 `package` 标识，CPU 使用真实 Linux `cpu` 编号。默认与 `-S` 仅输出汇总，
`-a` 增加 CPU 层级，`-s all` 还增加插槽层级。显式选择决定层级：`-s pkg`（或
`package`）和 `pkg_freq_mhz` 等精确字段按需输出插槽行。CPU 的 package 拓扑仍可
用于身份识别与绘图分组。

缺失数字/字符串与非有限值为 `null`，可用 Boost 为布尔值，字符串按 JSON 转义。
正常退出时闭合数组；异常终止可能留下不完整文档。

### CSV

首次表头定义布局；默认输出使用仅汇总布局：

| 所选层级 | 身份列 |
|---|---|
| 仅汇总 | `Scope`，值为 `SUM` |
| 仅插槽 | `Package` |
| 仅 CPU | `CPU`，真实 Linux 编号 |
| 混合 | `Scope,CPU,Package`；每区间按 `SUM`、`PKG`、`CPU` 排行 |

单层级数据列用 JSON key，混合列添加 `summary.`、`package.`、`cpu.` 前缀。
PMU 列为 `pmu.<event>`，混合时为 `summary.pmu.<event>` / `cpu.pmu.<event>`。
缺失及不属于该行层级的单元格为空，Boost 用 0/1。逗号、双引号、CR、LF 按 CSV
规则转义；使用标准解析器，并按身份筛选行。

输出层级和有序列须与首次表头一致。能力变化改变布局时，在写入该区间前返回
错误；CPU 成员变化且布局不变时可继续。字段可能变化时，重新采集 CSV 或使用 JSON。

### 输出生命周期

`-o`/`--out` 指定统计文件。周期统计默认输出到 stdout，命令统计默认输出到
stderr，子进程保留原有 stdout/stderr；默认统计会与子进程 stderr 共流，命令
JSON/CSV 应写入文件。命令模式要求显式 `--`，拒绝显式 `-i`/`-n`、`-D`、
`--probe` 和 `-l`。

已有输出目标只在初始化与基线采集成功后打开；区间输出还要求格式化缓冲区分配
成功。启动成功后逐步写入；后续失败保留已写前缀并返回非零状态。所有成功操作，
包括 help/list/probe，
都要求 stdout 刷新检查成功。SIGPIPE 按 EPIPE 处理，使资源清理后再退出。

### CLI 迁移

规范选项为 `-n`/`--num_iterations`、`-N`/`--header_iterations`、
`-S`/`--Summary`、`-J`/`--Joules`、`-o`/`--out` 和 `-D`/`--Dump`。
长选项区分大小写，旧拼写与 `-O` 不再接受。探测只用 `--probe`，`-P` 现禁用
perf。`-q` 保留文本表格，原来无装饰的文本输出改用 `--quiet-all`。

CLI 采用部分 turbostat 惯例，保留原生视图、格式和一秒区间。`Freq` 是 sysfs
瞬时采样，不是 Avg_MHz/Bzy_MHz；默认 Busy 来自软件 procstat 记账，不是硬件
C0。LPI 列与 Idle% 独立。功耗为 mW，`-J` 在旁边增加估算的区间焦耳数。README
提供简短迁移表。

## 导出画图

绘图需要 Python 3 与 matplotlib，例如在激活的环境中使用：

```bash
python3 -m venv .venv
. .venv/bin/activate
python3 -m pip install matplotlib
armstat -S -a -f json -o summary.json -i 1 -n 60
armstat -a -f csv -o cpus.csv -i 1 -n 60
armstat-plot-summary summary.json --y power --y2 temp0
armstat-plot-cpu cpus.csv --preset busy --top 8
```

源码入口为 `python3 scripts/plot_sum.py`、`python3 scripts/plot_cpu.py`。两者
也接受包含所需层级的混合 JSON/CSV。使用各自的 `--help` 和输入的 `--list-fields`。

| 选项 | 用途 |
|---|---|
| `--preset`，或 `--y` / `--y2` | 选择预设或字段，两种方式不混用 |
| `--sample-range START:END` | 从 1 开始的样本窗口；CSV 只保留所选窗口，JSON 全量加载 |
| `--smooth N` | N 个样本的滑动平均，当前样本缺失仍保留空缺 |
| `--cpu-filter`、`--top`、`--group-by node/core` | 限制 CPU 曲线或分组 |
| `--aggregate avg/max/min`、`--rank-by avg/max/last` | CPU 分组聚合与前 N 项排名 |
| `--output`、`--output-dir`、`--format png/svg/pdf` | 输出位置和图片格式 |

按 core 分组使用 `(package, core)`，避免不同插槽同编号合并。平均排名是可见
样本的算术平均，不按时长加权。没有主字段数据的对象报告后跳过；只缺副字段
时仅跳过该副曲线。

加载器拒绝不支持或非整数 schema、重复 JSON key/CPU ID、不合法层级、非标准
数字常量、不连续区间、重复 CSV 列和行宽不符。超出浮点范围的数字变为空缺；
缺失数据与平滑不会用旧样本替代。

只有所选时间戳全部有效且递增时才使用真实时间，否则整条轴改用样本编号。
保留导出时区，窗口内偏移变化时转为 UTC。Python 3.10 也支持九位小数与 `+0800`
等紧凑偏移，原始时间戳元数据仍保留。

字段与坐标轴标明标准单位；同单位的长标签使用 `Value (unit)`，图例保留字段名。
图例空间按渲染结果测量，汇总调色板覆盖全部十条 idle/LPI 曲线。无界面渲染先
写入目标旁的临时文件，成功后原子替换；保留旧文件权限，新文件使用 `0666 & ~umask`。
目录或渲染失败给出简洁错误，并保留已有输出。

## 构建与验证

| 命令 | 用途 |
|---|---|
| `make`、`make clean` | 发布构建/清理所选输出目录 |
| `make test` | C、CLI、导出、加载、流式处理，可选渲染和构建安装检查 |
| `make debug`、`make debug-test` | ASan/UBSan 构建/完整检查 |
| `make analyze` | GCC 路径敏感分析或 Clang 分析器 |
| `make target-test` | ARM64 Linux 运行与能力检查 |

```bash
make O=/tmp/armstat-build
/tmp/armstat-build/armstat --version
make CROSS_COMPILE=aarch64-linux-gnu-
ARMSTAT_REQUIRE_PLOT_RENDER=1 make test
```

可独立构建，也可放在 Linux `tools/power/armstat` 下。`O` 隔离二进制、对象、
依赖文件和测试。配置指纹包含编译器/目标、参数与 `VERSION`，防止切换构建时
复用旧产物；所选二进制原子发布。Linux 发布构建启用栈保护、PIE、RELRO 与
立即绑定。分析器产物独立，不替换发布二进制。

测试禁止写入 Python 字节码缓存。`make O=... clean` 保留源码缓存，源码目录内
clean 则删除它们。只读源码配合可写 `O` 也能构建和测试；除退出状态外，还应核对
实际产物。

### ARM64 目标机验收

在每个鲲鹏部署型号上先检查 `--probe`，并按部署权限运行。Busy/Idle 一致性是
必检项，声明用于部署的型号还须强制检查频率；其他能力仅在平台承诺提供时启用：

```bash
./armstat --probe
ARMSTAT_REQUIRE_FREQ=1 make target-test
ARMSTAT_REQUIRE_FREQ=1 ARMSTAT_REQUIRE_PMU=1 \
ARMSTAT_SOAK_ITERATIONS=1000 ARMSTAT_SOAK_INTERVAL=1 make target-test
```

| 强制项 | 检查能力 |
|---|---|
| `ARMSTAT_REQUIRE_FREQ` | 汇总频率为正，每个所选 CPU 至少一个正值样本 |
| `ARMSTAT_REQUIRE_PMU` | cycles、instructions 与 IPC |
| `ARMSTAT_REQUIRE_CPUIDLE` | 可见状态名映射、汇总驻留、CPU 层级至少一个有效 `lpi0_usage` 样本、绘图加载 |
| `ARMSTAT_REQUIRE_POWER`、`ARMSTAT_REQUIRE_TEMP` | 功耗/温度 |
| `ARMSTAT_REQUIRE_MEMBW`、`ARMSTAT_REQUIRE_UNCORE` | 内存带宽/uncore 频率 |

目标测试还检查选项、输出失败、探测结构、文本、JSON/CSV、时间戳、缺失值与
资源用量。稳定性采样检查 Busy/Idle 和所有强制能力的有效样本比例，默认至少 95%。
可调整 `ARMSTAT_MIN_VALID_PERCENT`、`ARMSTAT_MAX_RSS_KIB`、`ARMSTAT_MAX_OPEN_FDS`
和 `ARMSTAT_MAX_DIAGNOSTIC_LINES`。未设 FD 上限时默认为 256；强制 PMU 或首次
默认采样实际包含 IPC 时，使用 `max(256, tracked_cpus * 2 + 64)`，覆盖事件对
及其他运行 FD。未强制 PMU 且自动 IPC 不可用时仍保留 256，显式
`ARMSTAT_MAX_OPEN_FDS` 始终优先。

发布验收要求主机测试、静态分析、ASan/UBSan 检查、真实绘图、能力强制目标测试、人工
检视文本/汇总/JSON/CSV，以及物理服务器上的持续运行。VM/容器只能证明可移植性
与契约，不能证明物理 PMU、空闲、功耗、温度或带宽行为。

## 安装

```bash
sudo make install
make DESTDIR=/tmp/armstat-stage PREFIX=/usr install
make DESTDIR=/tmp/armstat-stage PREFIX=/usr uninstall
```

`PREFIX` 默认为 `/usr`。采集器和两个绘图命令位于 `bin`，共享绘图模块位于
`share/armstat`，man 手册位于 `share/man/man8`，中英文 README/参考、`VERSION`
和原样 `COPYING` 位于 `share/doc/armstat`。

## 文档维护

行为更新按以下顺序同步：

```text
代码 -> README.md -> README.zh-CN.md -> man/armstat.8
     -> docs/REFERENCE.md -> docs/REFERENCE.zh-CN.md -> tests
```

README 保存用法，本文保存完整公式/契约，man 手册保存参数，AGENTS.md 保存
维护规则。优先更新已有文档，精简时保留有意义的限制与失败语义。
