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
时间戳和稳定的 `schema_version`（当前为 8）。这是当前唯一支持的机器契约；
项目尚未正式上线，因此不保留历史导出格式兼容。内置画图脚本，可以从
`armstat -f json -O data.json` 直接生成时序图表，无需手动数据处理。
详见[综合参考](docs/REFERENCE.zh-CN.md#输出契约)。

## 支持平台

首发支持平台为 **华为鲲鹏 ARM64 Linux 服务器**。通用 Linux 数据源可能让其它
ARM64 机器也能显示部分基础指标，但它们不属于首发支持声明。具体鲲鹏服务器型号在
生产部署前仍须通过带能力强制项的目标测试与稳定性运行。

## 文档导航

- **[REFERENCE.zh-CN.md](docs/REFERENCE.zh-CN.md)** - 架构、导出契约、画图与发布验证
- **`armstat(8)`** - 安装后的命令行手册（`man armstat`）

## 快速开始

采集器面向 ARM64 Linux，需要正常挂载 `/proc` 与 `/sys`。构建需要 C 编译器和
`make`；画图功能是可选项，需要 Python 3 与 matplotlib。

```bash
make
./armstat --probe
./armstat -i 1 -n 5
./armstat -a -i 1 -n 5
./armstat -S -a -f json -O armstat.json -i 1 -n 5
sudo make install
```

安装后还会提供 `armstat-plot-summary` 与 `armstat-plot-cpu`；只有实际画图时
才需要 matplotlib。

判断可选字段缺失是否为缺陷前，应先检查 `--probe`。PMU/IPC 通常需要 root 或
宽松的 `perf_event_paranoid`。生产部署前，执行
[REFERENCE.zh-CN.md](docs/REFERENCE.zh-CN.md#arm64-目标机验收) 中带能力强制项的
目标机验收。

## 当前输出模型

`armstat` 当前使用 `SUM + package + CPU 行` 模型：

- 默认输出是简洁聚合视图：一行 `SUM` 加每个 package 一行
- `-a` 打开所有支持的基础列组，并在默认聚合视图下展开 per-CPU 行
- `-S` 每个 interval 只输出一行 `SUM`
- JSON 输出 interval 对象数组
- CSV 根据所选字段输出 summary-only、package-only、CPU-only，或带明确
  scope 的混合行

普通文本模式会在每个多行采样块前输出明确的 `--- interval N ---` 标记，并在
相邻采样块之间留一个空行。Summary-only `-S` 仍保持每个 interval 一行；`-q`
会连同启动 banner 和表头一起抑制该标记，便于紧凑的文本管道处理。

它是“ARM 上的 turbostat 风格工具”，但不是 x86 `turbostat` 的
逐列等价实现。

## 理解一轮输出的心智模型

理解 `armstat` 最简单的方式是把它看成“区间统计器”：

1. 先建立一份 baseline
2. 等待一个完整 interval
3. 再采一份新快照
4. 对两次快照做区间 delta / 百分比计算
5. 最后格式化成 `SUM`、per-package 行、per-CPU 行，或所选组合

因此，第一条可见输出永远代表“一个完整区间”，而不是程序启动瞬间的状态。

后续样本使用锚定到 baseline 的 monotonic-clock deadline。正常采集/格式化开销
会从下一次等待中扣除，不会逐轮累积成节拍漂移；如果工作耗时超过整个 interval，
armstat 会跳过错过的 deadline，而不是突发追赶采样。指标公式始终使用实测 delta。

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
- 当前构建可表示的 Linux CPU ID 为 `0..1023`；如果 sysfs 报告了更多在线
  CPU 或更高编号，armstat 会保留真实在线数用于诊断、打印采样截断警告，并且
  只采样可表示的 CPU ID

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
  时间占比；Linux 的 iowait 属于 idle 计数，因此已包含在 `Idle%` 中，
  不会算作 busy
- 分 idle state 驻留列和 usage rate 列使用 cpuidle `stateN/name`，例如
  `LPI-0`、`LPI-1`……以及 `LPI-0_usage`（`usage` 区间增量/秒）
- cpuidle 只用于拆分 `LPI-*` 驻留，不作为 Busy/Idle 的权威来源
- 当没有 cpuidle 数据时，分 state 列会自动隐藏
- cpuidle 计数瞬时失败或回退时，可见 LPI 集会保持不可用，直到重新建立
  baseline
- state 缺失或被禁用时，其 usage rate 为不可用，而不是 `0`
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
- 启用 `--cpu` 时，默认聚合视图仍然保留，并基于过滤后的 tracked CPU 集计算
- 聚合 section 只有在会与 filtered CPU 行隐式混排时才被抑制；像
  `-s pkg_freq_mhz --cpu 0-3` 这样的显式 aggregate-only 请求仍会输出，
  并基于过滤后的 tracked CPU 集计算
- 使用 `--cpu` 时，tracked-CPU-derived 的 SUM 字段（如 frequency、idle、LPI、PMU）基于过滤后的 tracked CPU 集；平台/全局字段保持其自然 summary scope

### nohz_full 与 Busy/Idle

`nohz_full` CPU 在短 interval 下更容易让 `/proc/stat` 的 Busy/Idle
看起来抖动。因此默认的 `auto` busy-source 会在
`/sys/devices/system/cpu/nohz_full` 指定的 CPU 上优先使用
`/proc/schedstat` 的运行时间记账，而在其它 CPU 上继续沿用 `/proc/stat`。

schedstat 读取器按内核文档解析 9 个 CPU 字段，并使用第 7 字段（任务运行
总纳秒数）。仅接受已知的 schedstat 版本 10–17；未知版本会安全回退到
`/proc/stat`，不会猜测字段布局。

### 功耗与温度

当前实现优先适配如下平台模型：

- package 功耗来自唯一可判定的 `hwmon` `name=power_meter`，使用其
  `power1_average`
- summary 温度来自 `thermal_zoneN/temp`，在当前 `thermal-zone-index`
  策略下，其中 `N` 对应 NUMA/Vdie `N`

因此：

- `Power` 是 `SUM` 级字段，单位为 mW
- CPU 行的 `Temp` 来自该 CPU 所属 NUMA 节点温度，单位为 C
- `Temp0` ... `Temp3` 只在发现对应 NUMA/Vdie zone 后显示，单位为 C
- 稀疏 NUMA ID 会保持原编号（例如 node 2 映射到 `Temp2`），并支持有符号
  毫摄氏度读数
- 目前不暴露 per-core power / per-core temperature

summary 温度策略现在是显式的，而不是隐含假设：

- 默认策略：`thermal-zone-index`
- 行为：`thermal_zoneN/temp -> TempN -> NUMA/Vdie N`
- 覆盖方式：`ARMSTAT_TEMP_POLICY=none` 可禁用 summary `TempN` 发现
- 可用值为 `thermal-zone-index`、`none` 和 `disabled`；未知值会作为启动错误，
  不会静默丢失温度数据

`--probe` 会打印当前生效的 `summary_temp_policy`，以及 package 功耗和内存带宽
来源的候选数量。候选多于一个时会作为歧义禁用，不会任意采用目录遍历遇到的
第一个来源。

### PMU

PMU 通过 `perf_event_open()` 实现：

- 每个 tracked CPU 单独建一组 perf counters
- 同一 CPU 上的事件按 perf group 打开
- group read 包含 `time_enabled` / `time_running`
- 多路复用事件会在导出 interval 值前做 scaling
- `-I` 会启用 `cycles,instructions` 并打开 IPC 列
- `-p ...` 只启用 PMU 计数器，不自动显示 IPC
- 只有事件列表同时包含这两个命名事件且本区间 cycles 非零时，IPC 才可用
- 未知或重复 PMU 事件名以及超过 `MAX_PMU_EVENTS` 的事件列表立即失败；机器
  输出的 PMU 对象以事件名为键，因此不允许重名；perf 权限/打开失败仍降级为
  可见的不可用值
- 首次 group read、短读/失败、零运行时间或计数器回退都属于不可用 interval；
  恢复时先重新建立 baseline，不会把故障窗口压缩进一条 delta
- PMU 文件描述符使用量同时随事件数和 tracked CPU 数增长；打开 group 前，
  armstat 会在现有 hard limit 内尽力提高 soft `RLIMIT_NOFILE`，预算仍不足时
  会告警，减少 CPU 或事件任意一项都能降低 fd 压力

PMU 一般需要 root 权限，或者较宽松的
`/proc/sys/kernel/perf_event_paranoid`。

如果显式请求了 PMU 或 IPC 列，但没有通过 `-p` 指定事件列表，
armstat 会默认使用 `cycles,instructions`。

ARMv8 raw 别名使用架构 PMUv3 事件号。其中 `mem-read` / `mem-write` 分别
统计 retired load / store；它们是事件次数，不是传输字节数或内存带宽。
可选 cache 层事件是否实现仍取决于具体 CPU，因此架构事件名在某台机器上仍
可能不可用。

## 架构

源码树按四条职责边界组织：

```text
src/app/       命令行解析与进程生命周期
src/core/      采集编排、区间聚合与 CPU inventory
src/platform/  Linux / ARM 遥测后端
src/output/    字段注册、中间记录以及 text/JSON/CSV serializer
```

详细职责与数据流统一维护在
[REFERENCE.zh-CN.md](docs/REFERENCE.zh-CN.md#运行架构)，README 不再保留第二份
容易过期的逐文件架构清单。

## 优化策略

当前实现的优化目标是“在大核数 ARM 服务器上把监控器自身的扰动压低”，
而不是每轮无差别读取所有数据源。

### 1. 三层采样模型

- **Static / rebuild 层**：
  CPU inventory、topology、传感器发现、cpuidle state 名称、PMU 事件元数据
- **Slow-changing 层**：
  CPU min/max 频率、governor、boost 与 cpuidle `disable`
- **Per-interval fast path**：
  按当前字段选择读取当前频率、`/proc/stat` delta、package 功耗、NUMA 温度、
  PMU 计数和 cpuidle `stateN/time` 或 `stateN/usage`

这样“平台上有什么”这类慢变化工作就不会落到每轮热路径里。

hotplug 检测每个 interval 只读取紧凑的 Linux `online` CPU mask。当可表示数量、
真实总数和具体成员都与缓存 catalog 一致时，armstat 跳过目录枚举；只有真实变化
或 mask 无法读取时，才执行完整 inventory 扫描和连续两次确认去抖。

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

- 只有显示频率字段时才读取 per-CPU 当前频率及其 slow-changing cpufreq 元数据
- 只有 Busy/Idle/IOWait、LPI residency 或系统计数字段需要时才解析
  `/proc/stat`；只选择 usage 时不依赖它
- cpuidle 只读取所选 state 与所需计数器类型：summary residency 不读取
  `stateN/usage`，只选择 `LPI-N_usage` 时不读取 `stateN/time`
- 只有有可见字段依赖 package power 时才读取功耗
- 只有显示温度字段时才读取温度
- 只有 PMU active 时才读取 PMU

记录物化也遵循同一原则：summary-only 与 package-only 输出不会分配或填充
per-CPU 输出行。

### 5. PMU grouping + scaling

PMU 以 tracked CPU 为单位建立 perf group。group read 会拿到
`time_enabled` / `time_running`，多路复用时先做 scaling，再导出 interval
delta。每个 CPU 独立保留 group 有效性，只有所有 tracked CPU 都提供完整
interval 时，summary PMU 才可用。

### 6. 三部分输出流水线

输出分成三步：

1. `columns.c` 持有列可见性（`show_*` 标志）与字段描述符表
2. `formatter_record.c` 构造稳定的 `interval_record`，`formatter_values.c`
   提供类型化字段 getter
3. `formatter_text.c`、`formatter_json.c`、`formatter_csv.c` 负责序列化，
   `formatter_machine.c` 保存机器输出共用辅助逻辑

这样 text / JSON / CSV 共用同一套字段模型，不会在 serializer 里重复计算。

## 构建

```bash
make
make test
make debug-test
make analyze        # Linux 上使用 GCC 静态分析器
make target-test    # ARM64 Linux 主机运行态验收
make O=/path/to/output
```

本地构建可不安装画图库。如果发布门槛要求缺少 matplotlib 时测试必须失败，使用
`ARMSTAT_REQUIRE_PLOT_RENDER=1 make test`；CI 会安装 matplotlib 并启用该门槛。

armstat 可以从本仓库独立构建，也可以放入 Linux 源码树
`tools/power/armstat` 后用同样的 `make` 命令构建。交叉编译通过
`CROSS_COMPILE` 支持（例如 `CROSS_COMPILE=aarch64-linux-gnu-`），
外部构建通过 `make O=/path/to/output` 支持。binary、object、依赖文件和测试
binary 都会留在 `O` 下；release、sanitizer、自定义编译参数、compiler version
或 compiler target architecture 切换时，不兼容的旧 object 会自动失效。
object 和链接后的 binary 按构建配置指纹隔离，因此并行执行构建目标或快速执行
release/debug/release 切换也不会复用陈旧 object；选中的 binary 会原子发布到
`armstat`。
Linux release 默认同时启用 stack protector、PIE、RELRO 与立即符号绑定。
对外版本号统一来自仓库根目录的 `VERSION` 文件，并会进入构建指纹和安装文档。
`make install` 也会安装完整的 GPL-2.0 许可证正文。
`make analyze` 的 analyzer 专用 binary 会隔离在 `.armstat-analysis/`，不会清理
或替换普通构建选中的 release `armstat`。

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

`-n 0`（默认值）会一直运行到收到中断。`-D` 仍会等待并测量一个完整 interval，
并不是瞬时点采样。text 模式的 `-D` 会保留 banner 与列表头，使单次结果可以
自解释；确实需要无表头 text 时再显式添加 `-q`。

最小 interval 为一微秒（`0.000001` 秒）；更小的值会被拒绝，因为 collector
时间戳和导出 interval 使用微秒精度。text 启动 banner 会保留这种亚秒精度，
不会把合法短 interval 四舍五入成零。

### 其他选项

- `-N, --header-iterations N` — 每输出 N 行数据后重印一次 text 表头
- `-J, --joules` — 显示区间能量（焦耳）
- `-q, --quiet` — 抑制 text banner、表头和 interval 标记
- `-h, --help` — 显示完整命令行摘要并退出
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

collector 初始化和 baseline 采样成功前，已有输出文件不会被截断。运行态输出
仍保持 streaming，因此长时间采集成功启动后，下游可以边生成边读取。
所有成功路径（包括 `--help`、`--version`、`--list` 和 `--probe`）都会在返回
0 前 flush 并检查 stdout；文件系统已满、导出目标损坏或其他可检测写入失败会
返回非零状态。下游管道提前关闭时，`SIGPIPE` 会被转成可检查的 `EPIPE`，使
PMU 与缓存的 sysfs 资源可以在返回 1 前正常清理。

CSV 导出现在会在每行前面附带 `schema_version`、`interval`、真实采样窗口
`duration_us`、秒级 `timestamp`、纳秒级 `timestamp_ns` 和 RFC 3339
`timestamp_iso`，方便后处理脚本直接按真实时间对齐亚秒样本，并识别当前
导出契约版本。JSON 使用相同元数据。

当只输出 summary 时，schema 8 CSV 使用 `Scope` 表头与 `SUM` 值，而不是把
数据值写成列名。当同时选择多个 scope 时，CSV 使用 `Scope,CPU,Package` 身份列，
并输出 `SUM`、`PKG` 或 `CPU` 行。像 `-s pkg_freq_mhz` 这样的 package 精确
字段也会生成可用的 package-only 导出，不会只留下空文件。
所有 CSV 数据列都使用与 JSON 相同的稳定字段 key；只有 mixed-scope CSV 会再加
`summary.`、`package.` 或 `cpu.` 限定前缀。

更完整的 JSON/CSV 字段与结构说明见[中文综合参考](docs/REFERENCE.zh-CN.md#输出契约)
和[英文综合参考](docs/REFERENCE.md#output-contract)。

### 摘要模式

```bash
armstat -S
armstat -S -a
```

普通默认输出已经包含 `SUM` 和 package 行。`-S` 去掉 package 区域，只保留
summary；`-a` 在默认聚合视图下展开 CPU 行，并且不会隐式启用 PMU/IPC。
使用 `-s` 时只输出请求的层级：需要 CPU 行时包含 `cpu`，需要 package 行时
包含 `pkg`。

启用 `--cpu` 时，默认聚合行基于过滤后的 tracked CPU 集计算。如果显式展开
CPU 行，则隐式聚合行会被抑制，避免混合 scope 造成误解；需要明确的过滤后摘要时
使用 `-S --cpu ...`。

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

`-s` 与 `-H` 均会对未知列组或字段名报启动错误。完成能力发现后，如果所选
字段在当前模式下无法产生任何数据行（例如 `-S -s cpu`，或只选了平台上
不存在的动态字段），armstat 也会明确失败，不会静默输出空数据集。可用
`--probe` 与 `--list` 定位选择问题。`--list` 会同时列出每个精确字段的
scope、数据类型、单位、text 标签和 JSON 键，便于在写采集脚本前核对契约。

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

重复的 `-s` 会取并集。显式指标请求 `-p`、`-I`、`-J` 无论写在 `-s`
之前还是之后，也都会保留在该并集中；普通参数顺序不会静默关掉已经请求的
PMU、IPC 或 energy 输出。

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
- `mem-read`（架构 load-retired 事件）
- `mem-write`（架构 store-retired 事件）
- `l1d-cache-refill`、`l1d-cache`
- `l1i-cache-refill`、`l1i-cache`
- `l2d-cache-refill`、`l2d-cache`
- `l3d-cache-refill`、`l3d-cache`

原始 ARM PMU 事件配置也可以用十六进制值指定，例如 `0x11`。未知事件名和超过
`MAX_PMU_EVENTS` 的列表在采样开始前失败。如果事件已知但当前机器上 perf
不可用，请求的 PMU 列保留可见并渲染为不可用，而非报告假零。

### 辅助输出

```bash
armstat -l
armstat --probe
```

`-l` 会打印内置列组、每个可精确选择字段的 scope/text label/JSON key，以及
PMU 事件名。脚本中优先使用稳定 field ID，可以避免重名 label 的歧义。
`--probe` 会一次性打印当前平台的能力摘要，包括 CPU 拓扑、
effective busy-source 策略、cpuidle/LPI 可用性、温度节点 mask、实际选中的
package 功耗与内存带宽 sysfs 路径、候选数量与歧义说明，以及基础 PMU 可用性
探测。PMU 检查只会在
第一个 tracked CPU 上打开 `cycles`，这是低成本能力检查，不代表每个事件都能
在每个 CPU 上打开。key-value 输出包含 `probe_schema_version: 1`，部署脚本可
据此显式拒绝未来不兼容的 probe 契约。当存在 cpuidle 状态时，
`idle_state_N_name` 会把每个可见的 `LPI-N` 字段映射到 Linux 对应的
`stateN/name` 值。

### 画图

附带的画图脚本会在字段列表和坐标轴显示标准单位；时间轴保留导出记录中的
RFC 3339 时区偏移，不会悄悄改用画图机器的本地时区。summary 图使用 10 色
调色板，完整 `idle-lpi` preset 不会复用 4 种颜色；脚本使用无界面渲染后端，
只有图片完整生成后才原子替换目标文件。平滑不会抹掉不可用样本的断点。
在所选时间窗内完全没有主字段数据的 CPU 或分组会被明确报告并跳过，
不会生成只有图例的空线。在双轴 CPU 图中，只有主字段数据的实体仍会保留，
但其空的次轴线会被报告并跳过。完整说明见[综合参考](docs/REFERENCE.zh-CN.md#导出画图)。

### 导出契约

机器可读导出的字段和结构说明见[综合参考](docs/REFERENCE.zh-CN.md#输出契约)。

### 测试

测试说明见[综合参考](docs/REFERENCE.zh-CN.md#构建与验证)。

## 字段与作用域

当前字段模型明确区分 summary 级和 CPU 级。

### Summary 级字段

- `Freq`
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
    `/sys/devices/system/cpu/cpuN/cpufreq/cpuinfo_cur_freq`
  - 单位：
    MHz
  - 公式：
    `Freq = cpuinfo_cur_freq / 1000`

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
    属于 slow-changing 层字段；不可用时 text 显示 `-`、JSON 为 `null`、
    CSV 为空单元格，而不是空字符串

- **Boost**
  - 来源：
    优先 per-CPU `cpufreq/boost`，其次全局 `cpu/cpufreq/boost`
  - 值：
    text/CSV 使用 `1`、`0`；JSON 使用 `true`、`false`；不可用时分别为
    text `-`、JSON `null`、CSV 空单元格

- **summary/package 级 `Freq`**
  - per-CPU 采样：
    interval 结束时读取到的当前 `cpuinfo_cur_freq`
  - summary/package 公式：
    对该 scope 内所有有效 tracked CPU 的本轮采样值求平均
  - 备注：
    这是 cpufreq 采样值，不是基于硬件计数器计算的有效频率或忙时频率；读取失败
    时保持不可用

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
    它当作分 `LPI-*` 展示时要解释清楚的基准值。procstat 的 idle 值包含
    Linux 的 iowait 字段。

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
    `IOWait%` 是 `Idle%` 的子集，因此已经从 `Busy%` 中排除
  - 解读：
    当前模型满足 `Idle% + Busy% = 100`；`IOWait%` 是 idle 内部的参考拆分，
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
  - 失败处理：
    某个 state 计数不完整或回退时，该 CPU 的全部可见 LPI 值保持不可用，
    直到下一条完整 interval

### 功耗与能量

- **Power**
  - 来源：
    package 级 `power_meter/power1_average`
  - 单位：
    mW
  - 备注：
    当前实现只把唯一发现的 package 功耗源映射到 `SUM` 行，不在 CPU 行伪造
    per-core 功耗；零个或多个候选都会保持不可用

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
  - 说明：
    NUMA 节点 ID 可以稀疏，温度也可以低于 0 摄氏度

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
    唯一发现的平台相关原始内存带宽字节计数器
  - 公式：
    `(counter_now - counter_prev) / interval_seconds`
  - 单位：
    MiB/s（每秒字节数除以 1024²）
  - 备注：
    平台暴露零个或多个候选 counter，或唯一 counter 不可读时，`MemBW` 都为
    不可用；只有有效 interval 内字节增量确实为 0 时才输出 0

### PMU 与 IPC

- **PMU 事件列**
  - 来源：
    每个 tracked CPU 上的 `perf_event_open()`
  - 模型：
    per-CPU perf group，读取时带 `time_enabled / time_running`
  - 显示值：
    对 scaling 后的累计计数做 interval delta
  - 有效性：
    首次、失败、回退或未被调度运行的 group read 为不可用，并在下一条 delta
    前重新建立 baseline

- **Summary PMU**
  - 公式：
    对 tracked CPU 的 per-CPU scaled PMU 计数求和，再导出 interval delta
  - 有效性：
    只有全部 tracked CPU 都提供完整 group read 时才可用

- **IPC**
  - 公式：
    `instructions / cycles`
  - 作用域：
    同时存在命名事件 `instructions`、`cycles` 且本区间 cycles 非零时，
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
- 如果 package 功耗或内存带宽存在多个候选 sysfs 来源：
  - 该指标保持不可用，不会非确定性选择第一个来源或静默低估
  - `--probe` 会报告候选数量和歧义说明
- 如果通常可用的频率、Busy/Idle、LPI、功耗/能量、温度、内存带宽、系统计数
  或 PMU 发生瞬时读取失败：
  - text 输出 `-`，JSON 输出 `null`，CSV 输出空值
  - 累计来源在恢复时先重新建立 baseline，再输出新区间值，避免伪 0 和恢复尖峰
  - procstat jiffy delta 无法安全换算成微秒时会显示不可用，而不是溢出后伪造
    一个看似合理的 Busy/Idle 百分比
- 如果运行中发生 CPU hotplug：
  - inventory、sample cache、cpuidle 运行态、PMU、topology 会一起重建
  - 下一条样本会作为新的 baseline，避免跨 hotplug 边界混算 delta
- 如果 `nohz_full` 让短 interval 的 `/proc/stat` 抖动：
  - 默认 `auto` 会优先对这些 CPU 使用 `/proc/schedstat`
  - 拉长 interval 往往仍然更容易解释

## 平台说明

当前传感器策略假定：

- package 功耗要求恰好一个 `power_meter/power1_average` 候选
- 内存带宽同样要求恰好一个 `mem_bytes_read` 候选
- summary 温度遵循显式的 `thermal-zone-index` 策略：
  `thermal_zoneN/temp -> TempN -> NUMA/Vdie N`

例如：

- 1 路 / 2 NUMA 机器通常显示 `Temp0` 和 `Temp1`
- 2 路 / 4 NUMA 机器通常显示 `Temp0` 到 `Temp3`

如果你的平台暴露的是另一套传感器布局，可以先通过
`ARMSTAT_TEMP_POLICY=none` 关闭 summary `TempN` 发现，或者相应调整
`power_sensor.c` 中的 summary 温度策略。

## 当前限制

- 还没有像成熟 `turbostat` 那样的独立 core 聚合行；summary、per-package
  和 per-CPU 行已经实现
- per-core power 尚未实现
- CPU 行温度是 NUMA/die 温度映射，不是 per-core 传感器
- 当前固定大小采样数组无法表示 1023 以上的 CPU ID；armstat 会打印警告，
  并继续监控可表示的在线 CPU
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

GPL-2.0。完整许可证正文见 [COPYING](COPYING)。
