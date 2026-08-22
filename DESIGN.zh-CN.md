# ARMSTAT 设计文档

<p align="center">
  <a href="README.zh-CN.md">← 返回 README</a> |
  <a href="TESTING.zh-CN.md">测试</a> |
  <a href="EXPORTS.zh-CN.md">导出</a> |
  <a href="PLOTTING.zh-CN.md">画图</a>
</p>

本文描述的是**当前实现**，不是过去版本或未来设想。如果旧说明与代码不一致，
以代码和本文为准。

## 设计目标

`armstat` 的目标是成为一个 ARM64 服务器上的 `turbostat` 风格工具，
重点关注：

- 区间统计
- 稀疏 CPU 编号下的稳定 CPU 身份
- 大核数机器上的可接受采样开销
- 采集、聚合、输出的清晰分层
- 平台传感器差异与通用逻辑分离

## 非目标

当前 `armstat` 不追求复刻 x86 `turbostat` 依赖的
MSR / TSC / RAPL 一类硬件语义。

## 当前分层

```text
armstat_cli.c（CLI 解析）
  |
armstat.c（主循环 + 模块生命周期）
  -> collector.c
       -> cpu_inventory
       -> sample_cache
       -> idle_backend（仅策略辅助）
       -> cpufreq / cpuidle / power / pmu / sysstat 读取器
  -> aggregator.c
  -> columns.c（列可见性与字段描述符表）
  -> formatter_record.c（interval_record 构建；值 getter）
  -> formatter_text.c / formatter_machine.c（序列化，由 armstat.c 分发）
```

### armstat.c 与 armstat_cli.c

armstat_cli.c 职责：

- CLI 解析
- 列可见性选择（-s/-H/-a）
- 打印帮助和版本信息

armstat.c 职责：

- 按依赖顺序初始化模块
- 建立 baseline
- 驱动主循环
- 信号处理和优先级提升
- 清理资源
- --probe 一次性能力探测

关键行为：

- 第一条可见输出在完整 interval 之后产生
- 后续样本使用锚定到 baseline 的 monotonic deadline，正常 collector /
  formatter 开销不会累积成节拍漂移；超期时跳过错过的 deadline，不做突发追赶
- 与 clock I/O 解耦的 deadline 算术会直接单测相位保持、跨周期跳过和溢出拒绝
- baseline 或 interval 采集失败属于进程错误，不会输出半有效数据行
- 所有成功的提前退出与采样路径都会在返回 0 前 flush 并检查 stdout，使可检测的
  输出失败能传播给自动化调用方
- 忽略 `SIGPIPE`，使下游管道关闭也进入统一的 `EPIPE` 检查、模块清理与退出
  状态 1 路径
- `-S` 是 summary-only 模式
- `-a` 用来打开所有支持的列组
- `-D` 等价于跑 1 次，但不会隐式打开 quiet；除非显式使用 `-q`，单次 text
  输出仍保留自解释表头

### collector.c

职责：

- 管理统一 interval 时间戳
- 检测 CPU inventory 变化
- 触发 hotplug 重建链
- 编排一次区间采样

它应该保持“协调器”角色，不应堆积具体解析逻辑。

### sample_cache.c

职责：

- 提供 per-interval 内存池
- 实现慢变缓存刷新
- 读取快变原始值

采样层次：

1. Static / hotplug rebuild
2. Slow refresh（约 5 秒）
3. Per-interval sampling

当前在 cache 层采用的优化策略：

- 慢变 CPU 数据使用滚动游标和预算分摊刷新，而不是周期性全机重扫
- `/proc/stat` 的解析放在共享路径里，避免每个指标各自重复解析
- 只有在 `LPI-*` 可见时才刷新 cpuidle 分 state 数据
- 只有在可见字段确实依赖 package 功耗/温度时才去刷新这些传感器
- 正常 interval 采样前会尽力提升 armstat 自身优先级，减少监控器自带的调度抖动；
  提升失败不会影响功能正确性

`sample_cache.c` 中 slow refresh 的具体机制：

- `slow_init()` 为 `min/max/governor/boost` 这些慢变字段分配缓存，并把
  `slow_cursor` 归零
- 初始化后或 hotplug 重建后的第一轮，会走 `slow_update_all()`，先建立一份
  完整 baseline
- 后续 interval 则走 `slow_update_budgeted()`
- `slow_budget_for_interval()` 按下面的近似关系计算本轮预算：
  `budget ~= tracked_cpus * delta_us / target_sweep_us`
  其中 `target_sweep_us` 来自 `SLOW_TARGET_SWEEP_MS_DEFAULT`
- 算出的 budget 会被限制在
  `SLOW_MIN_CPU_BUDGET .. SLOW_MAX_CPU_BUDGET` 范围内
- 扫描窗口可通过 `ARMSTAT_SLOW_SWEEP_MS` 环境变量自定义
  （范围 100-60000 ms，默认 5000）
- `slow_cursor` 指向“下一次该从哪个 tracked CPU 开始刷新”，因此每轮只更新
  一个连续片段，然后推进游标
- cpuidle 的 `disable` 缓存通过
  `refresh_idle_state_disable_cache_budgeted()` 复用同样的预算式刷新模型
- `maybe_run_slow_refresh()` 被故意放在 fast-path 快照之后执行，这样 interval
  关键数据会先采，低频 housekeeping 不会拉长关键路径

这种设计的取舍是：不追求每轮都立刻刷新所有慢变字段，而是换取大核机器上
更稳定、可预测的采样开销。

它还负责保存每轮权威 Busy/Idle 的原始累计输入：

- `/proc/stat` `idle`
- `/proc/stat` `iowait`
- 按 busy-source 策略选择的 `/proc/schedstat` per-CPU runtime

这些值都先以 cumulative raw counter 的形式放进 `sys_snapshot`，
而不是在 collector 阶段直接转换成百分比。

快照中的 procstat idle 已包含 iowait，与 Linux 的记账模型一致。因此
`IOWait%` 是 `Idle%` 的解释性子集，不应再从 `Busy%` 中扣除。

### idle_backend.c

职责：

- 把 busy-source 策略集中放在一个地方
- 解析 `/sys/devices/system/cpu/nohz_full`
- 回答“某个 CPU 当前该使用 `/proc/stat` 还是 `/proc/schedstat`”

当前策略：

- 通过 busy-source 策略选择 `Idle%` / `Busy%` 的权威来源
- 默认 `auto` 模式在普通 CPU 上使用 `/proc/stat`，并在
  `/sys/devices/system/cpu/nohz_full` 指定的 CPU 上优先使用
  `/proc/schedstat` 运行时间记账
- `IOWait%` 继续来自 `/proc/stat`
- cpuidle 仅用于拆分 `LPI-*` 驻留；如果 cpuidle 不可用，则隐藏分 state 列

这个文件现在已经**不再**拥有一个运行时 idle backend 对象，也不再维护一套
隐藏的 delta 时间线。真正的区间百分比是在 `sample_cache.c` 捕获原始累计
计数之后，由 `aggregator.c` 统一推导出来的。

### aggregator.c

职责：

- 从原始快照计算 interval delta
- 计算平均 MHz、busy/idle、功耗、能量、membw、PMU、IPC
- 维护上一轮基线

聚合层不能直接做 sysfs/procfs I/O。

### 格式化输出栈

职责：

- 将 `sys_snapshot + interval_stats` 转换成 `interval_record`
- 将中间模型序列化成 text / JSON / CSV

采用三阶段：

1. `columns.c` 持有列可见性标志（`show_*`）、idle-state 与 summary-temp
   系列可见性及覆盖位掩码，以及字段描述符表（`all_fields[]`），将字段
   id 关联到其 group、scope、series、数据类型、单位、显示精度、enabled
   标志与值 getter。CLI 解析通过 `enable_*()` setter 写入可见性；
   sample_cache 读取以驱动按需采样。
2. `formatter_record.c` 构建稳定的中间模型——字段表引用的值 getter 位于
   此处，紧邻 record 模型。
3. serializer 只消费中间模型，不知道采样细节或列可见性内部。

`--list` 的精确字段发现也直接由同一 descriptor table 生成，包括类型与
单位元数据，因此不会与 `-s` / `-H` 实际接受的字段或 serializer 格式漂移。
第一个 `-s` 建立字段 whitelist，后续 `-s` 在其上取并集；显式 PMU（`-p`）、
IPC（`-I`）和 energy（`-J`）请求会重新并入 whitelist，使这些独立选项的
组合不再依赖命令行顺序。
collector 完成能力发现后，共享 section policy 会确认当前模式至少能输出一条
有意义的数据行。校验发生在打开目标文件之前，因此模式冲突或只含不可用字段的
选择不会静默生成空采集，也不会破坏旧文件。

## CPU 身份模型

这是最核心的设计点之一。

系统中同时存在两种 CPU 身份：

- `cpu_id`：真实 Linux CPU ID
- `tracked_idx`：内部连续数组下标

规则：

- 对外语义一律使用 `cpu_id`
- 内部数组一律使用 `tracked_idx`
- topology 查询一律接受 `cpu_id`
- CPU filter 一律按真实 `cpu_id` 表达
- `--cpu` 过滤在运行时采样开始前应用，影响 PMU/cpufreq/cpuidle/Busy-Idle 的 tracked CPU 集合
- 无效过滤 token（非法值、反向区间、无匹配）是启动错误

这样可以避免稀疏编号和 hotplug 把拓扑或输出语义搞乱。

## CPU Inventory

`cpu_inventory` 现在统一承担以下单一事实源职责：

- present CPU
- online CPU
- tracked CPU
- 每个 CPU 的拓扑属性缓存

同时它也继续提供 collector / formatter 依赖的兼容接口：

- `get_cpu_id_by_tracked_idx()`
- `get_tracked_cpu_count()`
- `check_and_rebuild_inventory()`

hotplug 检测基于真实成员变化，而不是只看 CPU 数量。
常见的无变化路径只读取并比较 `/sys/devices/system/cpu/online`，不会枚举每个
`cpuN` 目录。mask 不匹配或无法读取时才回退到完整 catalog 扫描；新的 tracked
集合仍须连续观察两次，才重建依赖运行态。

## Hotplug 重建链

当 CPU 成员集合变化时：

1. 重建 CPU inventory
2. 重建 cpufreq 与 sample cache
3. 重建 cpuidle 运行态
4. 如果 PMU 已启用，则重建 PMU
5. 重建 topology
6. reset aggregator
7. 消费当前样本作为新的 baseline，但不输出该样本

这样可以避免把 hotplug 前后的计数拼进同一个 interval。

## 数据模型

### 原始快照（`sys_snapshot`）

快照由 collector 持有，通过访问器函数暴露给消费者
（`sys_snapshot_get_effective_cpu_count`、`sys_snapshot_get_interval_delta_us`、
`sys_snapshot_get_cpu_truncated`、`sys_snapshot_get_freqs`、
`sys_snapshot_get_counters`）。多消费者字段通过这些 getter 读取，这样布局变更不会
波及 aggregator、formatter 和主循环。单消费者字段目前仍直接访问；未来一步可将
结构体完全 opaque 化。

包含：

- CPU 数量与截断元数据
- 原始 per-CPU 频率和可选 idle state 数据
- package 功耗
- NUMA 温度数组
- 原始 PMU 计数
- 原始系统计数（`struct raw_counters`，aggregator 也独立实例化为 `prev_counters`）
- 统一 interval 微秒数
- 频率、package 功耗、稀疏 NUMA 温度、内存带宽、系统计数、PMU、procstat
  与 schedstat 输入的当前样本有效性
- per-tracked-CPU procstat 与 schedstat 有效性标志，允许在
  `/proc/schedstat` 缺少某 CPU 数据时逐 CPU 回退到 `/proc/stat`

### 区间统计（`interval_stats`）

包含：

- interval 推导出的系统级统计
- per-CPU MHz / busy / idle / iowait
- 区间平均功耗与能量
- membw
- PMU delta 与 IPC

### 中间模型（`interval_record`）

`interval_record` 在每次 interval 构建时被完全实体化：它拥有所有 per-interval
动态值，`build_interval_record()` 返回后 serializer 不再解引用原始快照或
interval stats。

包含：

- interval 元数据
- summary 数据（`summary_data`）
- 自有的 per-CPU 行（`cpu_rows`）：freq 快照、busy/idle/iowait、IPC、
  分 idle state 驻留与唤醒、per-CPU PMU 计数、CPU 温度
- 自有的 per-package 聚合行（`packages`）
- 自有的 summary 分 idle state 驻留（`summary_idle_state_pct`）与 NUMA 温度
  （`numa_temps`）

静态身份字段（package、core、NUMA node）仍在输出时通过 tracked CPU id 从
topology 缓存惰性查询。

这样 text / JSON / CSV 就可以共用同一套字段表。

不可用的数值在 interval 与 record 模型中用 `NaN` 表示；serializer 分别将其
映射为 text 的 `-`、JSON 的 `null` 和 CSV 的空单元格。真实的计数器或传感器
零值仍输出为数值 `0`。

## 输出模型

当前输出按作用域分成：

- system-scope 字段
- CPU-scope 字段

当前 text 模型是：

- 默认模式：只打印 CPU 行
- `-a` 模式：`SUM` + per-package 聚合行 + CPU 行
- summary 模式：只打印 SUM 行

在混合作用域输出（`-a`）中，每个 section 各自携带自己的表头行、紧跟其数据行，
SUM / Pkg / CPU 三个 section 之间用空行分隔，避免三张表连成一片。

Package 行按 socket 聚合 per-CPU 的 MHz、Idle%、Busy% 和 IOWait%。只有在显式
启用 package 列组（`-a` 或 `-s package`）时才打印，默认输出保持只打印 per-CPU。
Core 级聚合尚未实现。

需要特别说明的 summary 语义：

- 功耗、membw、PMU、系统计数等属于 summary 级聚合字段
- `Idle%`、`Busy%`、`IOWait%` 属于 summary 级百分比
- summary 的 `LPI-*` 是“各 CPU 最终显示值”的平均，不会再单独做第二次
  全机 residual 计算
- 当启用 `--cpu` 过滤时，默认不会再自动混出 `SUM`，避免把过滤后的 CPU 行
  和全系统 summary 混在一起
- `--cpu` 下只有在聚合 section 会与 CPU 行同时输出时才抑制它；显式的
  aggregate-only 选择仍会显示，并基于过滤后的 tracked CPU 集计算
- 当 `--cpu` 过滤激活时，tracked-CPU-derived 的 summary 字段基于过滤后的 tracked CPU 集，而平台/全局字段保持其自然 summary scope

## Idle 模型

`Busy%` / `Idle%` 以 busy-source 策略选中的来源为权威。

默认 `auto` 模式的行为是：

- 普通 CPU 使用 `/proc/stat`
- `nohz_full` CPU 优先使用 `/proc/schedstat` 运行时间记账
- 如果某个 CPU 上 schedstat 不可用，则该 CPU 自动回退到 `/proc/stat`

`task-clock` 模式仍然保留为用户可见兼容选项，但当前等价于
`schedstat`。原因是 CPU 范围 perf `task-clock` 在目标 ARM 服务器上会
产生颠倒的 Busy/Idle 结果，因此当前实现不再把它当作权威 Busy/Idle 来源。

当 cpuidle 可用时：

- 总 idle 时间仍由选定的 procstat/schedstat 策略驱动 `Idle%` / `Busy%`
- `IOWait%` 也来自 `/proc/stat`
- 分 idle state 驻留列来自 cpuidle 的 state delta
- 分 state 唤醒列来自 cpuidle 的 usage 计数器（每秒进入次数）
- 分 state 列名来自 cpuidle `stateN/name`，例如 `LPI-0`、`LPI-1`
- 只显示真实存在的 state 列
- 最多显示八列 state（`LPI-0` ... `LPI-7`）
- 这些 `LPI-*` 列和权威 `Idle%` / `Busy%` 以及 procstat `IOWait%`
  共享同一 wall-clock 采样区间，但数据源不同
- 最深的“可用” state 会作为 residual bucket，用于补齐剩余 idle，
  使显示出来的 `LPI-*` 之和贴近权威的 `Idle%`
- 如果更深的 state 被 disable、不可用或超出可见列上限，residual 会上移到最深仍可见且可用的 state
- 如果任一可见且可用 state 的当前值或上一轮 baseline 无效，该 CPU 本 interval
  的所有可见分 state 值都不可用；恢复后的样本先建立新 baseline
- state 缺失、被禁用或不可使用时，其唤醒率为不可用，而不是合成 `0`

这意味着：

- `Busy%` 始终等于 `100 - Idle%`
- `IOWait%` 作为 `/proc/stat` 的独立指标单独展示
- 较浅层、可见的 `LPI-*` 会尽量保留 cpuidle 的原始驻留百分比
- 最深可见的 `LPI-*` 在需要时会做展示层调整，因此不一定等于该 state 的
  原始 cpuidle 百分比

当 cpuidle 不可用时：

- 分 state 列隐藏
- `Idle%` / `Busy%` 仍通过选定的 procstat/schedstat 策略正常工作

## 功耗与温度模型

当前平台适配模型是：

- package 功耗来自唯一发现的 `power_meter/power1_average`
- summary 温度来自 `thermal_zoneN/temp`，在显式的
  `thermal-zone-index` 策略下，其中 `N` 直接对应 NUMA/Vdie `N`

因此：

- `Power` 是由 package 级来源支撑的 `SUM` scope 字段，单位为 mW；不会出现在
  `PKG` 或 CPU 行
- 区间平均功耗使用前一轮和当前 package 功耗做梯形平均：`(prev + current) / 2`
- `Energy` 是当前 interval 的能量，不是进程生命周期累计能耗
- CPU 行温度来自该 CPU 所属 NUMA 节点
- thermal zone 编号可以稀疏；字段可用性由 sensor mask 决定，而不是仅由最大
  编号决定
- 保留带符号温度值
- summary 温度字段是 `Temp0` 到 `Temp3`，按实际发现显示，单位为 C
- per-core power 目前刻意不暴露

相关平台适配逻辑收敛在 `power_sensor.c`，避免污染 collector/formatter。
当前策略会通过 `--probe` 的 `summary_temp_policy` 输出暴露出来；如果平台
不满足这种 `thermal_zoneN -> TempN -> NUMA/Vdie N` 关系，可以通过
`ARMSTAT_TEMP_POLICY=none` 关闭 summary `TempN` 发现。可用值为
`thermal-zone-index`、`none` 和 `disabled`；未知值会使初始化失败，不会静默
切换到另一种传感器策略。
probe 还会输出已发现的温度节点 mask、package 功耗与内存带宽候选数量、歧义
说明，以及实际选择的 sysfs 路径，便于部署验收确认发现逻辑绑定了预期且唯一的
硬件源。该 key-value 契约用独立的 `probe_schema_version = 1` 标记版本。

## PMU 模型

当前 PMU 实现已经尽量向 `turbostat` 靠拢：

- 按 tracked CPU 打开计数器
- 同一 CPU 上的事件按 perf group 组织
- 读取使用 `PERF_FORMAT_GROUP`
- 收集 `time_enabled` / `time_running`
- 多路复用时，先对 interval 值做 scaling，再累计成虚拟累计值
- 聚合层再从这些累计值导出 per-CPU 和 summary delta
- 未知或重复事件名以及超过 `MAX_PMU_EVENTS` 的列表属于硬错误，确保 JSON
  对象键和 CSV 列名没有歧义
- 已知事件因 perf 权限或运行时限制无法打开时降级为 unavailable
- ARMv8 raw 别名使用架构 PMUv3 事件号：`mem-read` 与 `mem-write` 分别是
  load-retired 和 store-retired 事件次数，不是字节计数；可选 cache 事件在
  特定 CPU 上仍可能未实现
- 短读、读取失败、计数器复位或本 interval 没有 running time 时，该 CPU 的
  PMU 值在本轮不可用并重新建立 baseline；只有所有 tracked CPU 都有效时，
  summary PMU 才有效
- 只有命名事件 `cycles` 和 `instructions` 同时启用且相应 cycles delta 非零时
  才派生 IPC
- `--probe` 只在第一个 tracked CPU 上打开 `cycles`，避免分配完整 PMU 运行态；
  该结果有意只作为基础能力检查
- PMU 值是缩放后累计计数器模型的区间 delta，不是一次性原始读取

输出语义：

- 默认 CPU 模式显示每 CPU PMU 值
- summary 模式显示聚合 PMU 值
- IPC 同时支持 summary 和 per-CPU 视角

当前 caveat：

- PMU scaling 仍需在真实目标机上持续验证，尤其是高 multiplex 场景

## JSON 与 CSV

JSON / CSV 和 text 一样都来自同一套字段表。

当前行为：

- JSON 输出 interval 对象数组
- 默认模式输出 `cpus: [...]`
- summary 模式输出 `summary: {...}`
- 选择 package 输出时，JSON 输出 `packages: [...]`
- CSV 输出 summary-only、package-only、CPU-only，或带 scope 的混合行
- 混合 CSV 使用 `Scope,CPU,Package` 与 scope 前缀字段名，
  `SUM`、`PKG`、`CPU` 行不会混淆三个聚合层级
- schema 7 的 summary-only CSV 使用值为 `SUM` 的 `Scope` 身份列
- schema 7 元数据加入实测 `duration_us`；`timestamp_ns` 是首选时间轴，
  `timestamp` 为兼容性继续保留 Unix 整秒，`timestamp_iso` 是带 9 位小数的
  RFC 3339 字符串
- 机器可读导出使用 schema version 7；不可用数值和字符串在 JSON 中为
  `null`，在 CSV 中为空单元格；`Boost` 在 JSON 中是布尔值（不可用值规则
  从 version 5 开始）
- package 对象/行只序列化一次物理 package 身份；可选的 `pkg_id` 字段控制
  该身份是否输出，而不会再生成重复字段
- 每 interval 计数字段使用整数显示精度；速率和比率遵循字段 descriptor
  声明的精度

serializer 不应再编码采样假设；采样语义属于 builder 和 getter。

## 指标来源与公式

这是面向实现的“每个数字从哪里来”的摘要。

- `Freq`
  - 来源：`scaling_cur_freq`
  - 公式：`cur_freq_khz / 1000`
- `Min` / `Max`
  - 来源：`scaling_min_freq`、`scaling_max_freq`
  - 公式：`khz / 1000`
- `AvgFreq`
  - 公式：`(prev_cur_freq + cur_freq) / 2`，summary 再对 tracked CPU 求平均
- `UncoreFreq` / `uncore_freq`
  - 来源：设备名看起来像 uncore/interconnect/fabric，且唯一可判定的
    `/sys/class/devfreq/<device>/cur_freq`
  - 公式：`cur_freq_hz / 1000000`
  - 作用域：仅 summary
- `Idle%`
  - 来源：busy-source 策略选中的权威来源
  - 角色：当前采样区间内“总体 idle 预算”的权威值
  - 公式：
    - `procstat`：`/proc/stat idle`，再做
      `idle_delta_us / interval_delta_us * 100`
    - `schedstat`：先从 `/proc/schedstat` 的 per-CPU 运行时间得到
      `Busy%`，再由 `Idle% = 100 - Busy%`
    - `task-clock`：兼容别名，当前等价于 `schedstat`
  - procstat 细节：计算百分比前会将 Linux iowait 计数加入 idle
  - 实现路径：sample_cache.c 采集原始累计 idle/runtime 计数器，aggregator.c 导出区间百分比
- `IOWait%`（默认关闭；可通过 `-s iowait` 或 `-s IOWait%` 开启）
  - 来源：`/proc/stat iowait`
  - 公式：`iowait_delta_us / interval_delta_us * 100`
  - 实现路径：sample_cache.c 采集原始累计 iowait jiffies，aggregator.c 导出区间百分比
- `Busy%`
  - 公式：
    - `procstat`：`100 - Idle%`
    - `schedstat`：`sched_runtime_delta_ns / wall_clock_delta_ns * 100`
    - `task-clock`：兼容别名，当前等价于 `schedstat`
  - 备注：`IOWait%` 独立记录，不会再从 `Busy%` 中额外扣除
  - 实现路径：始终在 aggregator.c 中从与 Idle% 相同的区间 delta 计算
- `LPI-*`
  - 来源：`cpuidle/stateN/time`
  - 原始公式：`state_delta_us / interval_delta_us * 100`
  - 与 `Idle%` 的关系：
    原始 cpuidle 驻留只负责提供“idle state 怎么分布”的输入，不负责定义
    “总体有多 idle”
  - 展示规则：最深可见且可用的 state 会作为 residual bucket，
    用于让可见 `LPI-*` 总和贴近 `Idle%`
  - 可见列数：最多暴露八列（`LPI-0` ... `LPI-7`）
  - 含义：
    最终显示出来的 `LPI-*` 主要服务于解释权威的 `Idle%`，不保证保持
    原始 cpuidle 百分比完全不变
  - 实现路径：cpuidle.c 读取 stateN/time 计算原始 state delta，formatter 调整展示
- `Power`
  - 来源：唯一的 `power_meter/power1_average` 候选
- `Energy`
  - 公式：`interval_avg_power_mw * interval_seconds / 1000`
- `MemBW`
  - 来源：唯一的平台相关 `mem_bytes_read` 原始字节计数器
  - 公式：`(counter_now - counter_prev) / interval_seconds`
  - 单位：MiB/s（`bytes_per_second / 1024^2`）
- `CtxSw`
  - 来源：`/proc/stat ctxt`
  - 公式：`ctxt_now - ctxt_prev`
- `IRQs`
  - 来源：`/proc/stat intr`
  - 公式：`intr_now - intr_prev`
- `SoftIRQs`
  - 来源：`/proc/stat softirq`
  - 公式：`softirq_now - softirq_prev`
- `IPC`
  - 公式：`instructions / cycles`

## 降级模式规则

实现上刻意允许“部分能力缺失但整体仍可用”：

- 如果 `cpuidle` 不可用，`Idle%` / `Busy%` 继续工作，`LPI-*` 隐藏
- 如果 PMU 初始化失败，显式请求的 PMU / IPC 字段仍保留，但显示为不可用，
  而不是伪装成 `0`
- 如果功耗或温度传感器缺失，只影响依赖这些来源的字段，不影响整体 interval
  模型
- 如果 package 功耗或内存带宽存在多个候选 sysfs 来源，该指标保持不可用，
  不会依赖目录遍历顺序或冒险静默低估
- 频率、cpuidle、功耗、温度、内存带宽、procstat 或 PMU 瞬时读取失败时，
  只有依赖字段不可用；累计型来源恢复后会先重新建立 baseline，再恢复 delta
- procstat jiffy 到微秒的换算无法表示时会直接判为不可用，避免整数回绕生成
  看似合理的 Busy/Idle 百分比
- 如果运行中发生 hotplug 重建，下一条样本会作为新的 baseline，delta 不会跨
  重建边界计算

## 当前已知且接受的限制

- 还没有 core 聚合行（per-package 聚合已实现）
- 没有 per-core power 模型
- CPU 行温度是 NUMA/die 映射，不是 core-local sensor
- ARM 平台的硬件语义天然平台化，不可能逐列等价复刻 x86 `turbostat`

## ARM 平台适配

本章专门记录由 ARM 服务器平台特性和 Linux 内核接口驱动的设计决策。理解这些
对于将 armstat 扩展到新平台或排查意外输出至关重要。

### 为什么需要双源 Busy/Idle

ARM 服务器常用 `nohz_full` 在指定 CPU 上关闭调度 tick。当 tick 关闭后，
`/proc/stat` idle 计数器以粗粒度（可能达到秒级）更新，使短 interval 的
`Idle%` 看起来波动剧烈。

`/proc/schedstat` 运行时间记账由调度器独立维护，不受 tick 状态影响，能为
`nohz_full` CPU 提供亚 jiffy 精度的 busy 统计。这就是默认 `auto` 策略的动机：

- 普通 CPU 使用 `/proc/stat`（行为熟悉、久经考验）
- `nohz_full` CPU 优先使用 `/proc/schedstat`（不依赖 tick）

读取器只接受已知的 schedstat 版本 10–17，要求 CPU 记录包含完整 9 个字段，
并从第 7 字段读取任务运行时间。版本未知时整份 schedstat 不可用；某条 CPU
记录无效时只将该 CPU 标记为不可用，由逐 CPU 策略回退到 procstat。

procstat 与 schedstat 批量读取器都为每个 CPU ID 保留有效位。sample cache
只接收实际解析到对应行的值，因此稀疏或损坏的文件不会把缺失 CPU 行变成合成
零值。`/proc/stat` jiffies 使用 `_SC_CLK_TCK`（Linux USER_HZ）换算；只有该
用户态查询意外失败时才回退到 100。

`schedstat` 路径从 runtime delta 计算 `Busy%`，再通过 `Idle% = 100 - Busy%`
推导 idle。由于 schedstat runtime 在 `nohz_full` CPU 上可能因粗粒度的 tick
记账而偶尔超过 wall clock，aggregator 会将 `busy_us` clamp 到 `delta_us`，
剩余部分视为 idle。这个 clamp 是刻意的，长期平均值是正确的；除非内核的
schedstat 粒度发生改变，否则不应移除此 clamp。

### 为什么需要 LPI-* 残差调整

ARM cpuidle 的 `stateN/time` 计数器仅在 CPU **退出**空闲状态时推进。如果
一个 CPU 在 interval 开始时进入最深 idle 状态，并且在整个 interval 内从未
退出，cpuidle 中该状态的 delta 将为**零**——即使该 CPU 整个 interval 都在
idle。

相比之下，`/proc/stat` 能正确地把这段时间计入 idle。

残差调整正是为了弥合这一差距。它位于 `idle_display.c` 中，是一个纯函数
（`compute_idle_state_display`），接收原始 cpuidle 百分比、权威 `Idle%` 和
可见性数组，输出调整后的显示百分比：

- **浅层状态**（进出频繁）：保留原始 cpuidle 驻留百分比，因为计数器更新
  频繁，数据可靠
- **最深可用状态**：吸收浅层状态未能覆盖的剩余 `Idle%`，实际上捕获了
  "持续驻留未退出"的时间

这样得到的可见 `LPI-*` 列之和等于权威的 `Idle%`，同时在 cpuidle 最可靠
的层面保留了 ARM 特有的分状态 idle 信息。formatter 最多暴露八列 LPI。
残差归入最深的**可见且可用**状态；如果该状态被禁用（`stateN/disable = 1`），
残差上移到次深可用状态。

将规则提取到独立模块意味着 summary 平均路径不再需要在栈上合成临时
`struct cpu_row` 仅为复用该函数——它直接用 `double[8]` 输出缓冲区调用
`compute_idle_state_display`。

没有这个调整，在短采样 interval 下 `sum(LPI-*)` 会系统性地低估 `Idle%`，
使分状态 idle 列产生误导。

### 传感器模型假设

ARM 平台通过 `sysfs` / `hwmon` 暴露功耗和温度传感器，命名方式因平台而异。
armstat 做出以下假设——它们在很多 ARM 服务器平台上成立，但**不是内核保证的**：

**功耗**：
- 查找 `name = power_meter` 的 `hwmon` 设备
- 读取 `power1_average` 作为 package 级别功耗
- 仅在恰好一个匹配源存在时启用；零个、多个候选或读取失败时，功耗和能量
  为不可用，而不是 `0`

**内存带宽**：
- 在 `/sys/class/memory/mem*` 下查找 `mem_bytes_read`
- 仅在恰好一个候选存在时启用；不会猜测多个候选能否安全相加

**温度**：
- 默认 `thermal-zone-index` 策略将 `thermal_zoneN/temp` 直接映射到
  NUMA/vdie `N`（即 thermal zone 编号 = NUMA 节点编号）
- 发现过程保留稀疏 sensor mask，缺少低编号 zone 不会移动或虚构其它 NUMA 映射
- 接受带符号的毫摄氏度读数
- CPU 行温度来自该 CPU 所属 NUMA 节点的温度
- 此映射是平台约定，而非内核保证：thermal zone 编号取决于 probe 顺序，
  与 NUMA 拓扑没有固有联系
- 如果平台上的映射不正确，设置 `ARMSTAT_TEMP_POLICY=none` 禁用
  summary `TempN` 发现，或使用 `--probe` 检查实际映射

**Uncore/DevFreq**：
- 扫描 `/sys/class/devfreq/`，寻找设备名暗示为
  uncore/interconnect/fabric 时钟源的设备
- 找不到时静默回退

所有传感器策略集中在 `power_sensor.c`，未来平台可以替换发现逻辑而
不触及 collector/formatter 管道。

### 共享 sysfs I/O 原语

所有 sysfs/procfs 单值读取和缓存 fd 读取都通过 `sysfs_util.c`
（`sysfs_read_int_checked`、`sysfs_read_ull_checked`、`sysfs_read_str`、
`sysfs_path_exists`、`fd_read_int_checked`、`fd_read_ull_checked`）。这合并了此前在 `topology.c`、
`power_sensor.c`、`cpufreq.c`、`cpuidle.c` 中以不同错误约定复制的
`fopen`/`fscanf`/`fclose` 和 `lseek`/`read`/`strtoull` 模式。所有数值读取器
会拒绝溢出、无符号负数以及尾随非空白字符，并使用 checked 约定（成功返回
0，失败返回 -1，值通过 out-param 传出），因此调用者可独立跟踪有效性，而不
必占用任何合法数值作为错误哨兵。

### 文件描述符预算

armstat 为了性能在 interval 之间保持文件描述符打开：

| 子系统   | 打开 fd 数量               | 上限          |
|----------|---------------------------|---------------|
| cpuidle  | 每 CPU × 每 state         | ≤ 32（硬上限） |
| sysstat  | 2（`/proc/stat`、`/proc/schedstat`） | 2    |
| cpufreq  | 每 CPU 1 个（`scaling_cur_freq`）   | ≤ 16（硬上限，超出回退慢路径） |
| PMU      | 每事件、每 tracked CPU 1 个 fd（按 CPU 分组） | 无显式上限 |

在 256 个 tracked CPU 配 3 个 PMU 事件的机器上，仅 PMU 就需要 768 个 fd。
打开 group 前，armstat 会尽力把 soft `RLIMIT_NOFILE` 提高到事件 fd 需求加
运行态余量，但绝不会超过现有 hard limit。可用 limit 仍不足时会告警；此时可
提高 `ulimit -n`、减少事件，或用 `--cpu` 减少 tracked CPU 数量。

cpuidle fd 缓存被刻意限制在 32，防止 LPI 分状态报告消耗全部进程 fd
预算。PMU fd 不做限制，因为 PMU 是显式 opt-in 的——在大机器上启用 PMU
的用户隐式接受了 fd 成本。

### MAX_CPUS = 1024

`MAX_CPUS` 是 `collector.h` 中的编译期常量，同时限定最大 tracked CPU 数量
和可表示的 Linux CPU ID 范围（`0..1023`）。发现层会把真实 present/online
数量与固定 catalog 分开保存，因此遇到更多 CPU 或更高编号时会打印准确的
截断警告，而不是静默把机器显示得更小；采样随后继续覆盖可表示的在线 ID。
调高此值会增大整个代码库中所有 `MAX_CPUS` 尺寸的静态数组；修改前务必检查
所有栈和堆分配。

## 文档维护规则

一旦行为变化：

1. 更新代码
2. 更新 `README.md`
3. 更新 `README.zh-CN.md`
4. 更新本文与 `DESIGN.md`
5. 更新 `armstat.8`
6. 更新 `EXPORTS.md` 与 `EXPORTS.zh-CN.md`
7. 更新 `PLOTTING.md` 与 `PLOTTING.zh-CN.md`
8. 更新 `TESTING.md` 与 `TESTING.zh-CN.md`

所有文档必须保持一致。
