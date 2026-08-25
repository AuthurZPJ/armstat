<!-- SPDX-License-Identifier: GPL-2.0 -->
# armstat 综合参考

<p align="center">
  <a href="../README.zh-CN.md">README</a> |
  <a href="REFERENCE.md">English</a> |
  <code>armstat(8)</code>
</p>

这是 `armstat` 唯一的维护者与集成参考。README 负责日常使用说明；本文集中保存
架构、导出契约、画图和发布验证细节，避免把同一项目拆成大量专题文档。

## 仓库布局

```text
src/app/       命令行解析与进程生命周期
src/core/      采集编排、区间聚合与 CPU inventory
src/platform/  Linux / ARM 遥测后端
src/output/    字段注册、中间记录以及 text/JSON/CSV 输出
scripts/       导出加载器与画图工具
tests/         主机无关回归测试与 ARM64 目标机验收
man/           安装后的 man 手册
docs/          本综合参考
```

项目头文件放在对应实现区域旁边，属于内部接口；当前不提供公开的 C 库 API。

## 运行架构

采样主路径分为三阶段：

```text
采集原始快照 -> 计算区间 delta -> 构造记录并序列化
```

- `src/app/armstat_cli.c` 在运行时初始化前解析参数与列选择。
- `src/app/armstat.c` 管理启动、基于 monotonic clock 的采样循环、信号、输出文件
  收尾和退出清理。
- `src/core/collector.c` 编排一份原始快照，不负责用户可见百分比或格式化。
- `src/core/aggregator.c` 把累计值转成一份 `interval_stats`。公开入口保持为短而
  有序的流水线：初始化、频率、Busy/Idle、系统指标、PMU、package 汇总，最后
  提交下一轮 baseline。
- `src/output/formatter_record.c` 把快照与统计量复制进自包含的
  `interval_record`。
- `src/output/formatter_values.c` 提供字段注册表使用的只读 getter，与记录分配和
  物化分离。
- `src/output/formatter_text.c`、`formatter_json.c`、`formatter_csv.c` 分别负责
  三种序列化；`formatter_machine.c` 只保留 JSON/CSV 共用辅助逻辑。

第一次采集只建立累计计数器 baseline。等满一个完整区间后才产生第一条可见
样本。采样 deadline 锚定 monotonic clock：正常采集与输出耗时会从下一次等待中
扣除；如果整轮 deadline 已错过，则跳过它而不是突发追赶。所有区间公式使用实测
`interval_delta_us`，而不是用户请求值。

### CPU 身份

项目有意保留两种 CPU 身份：

- `cpu_id` 是真实 Linux CPU 编号，用于内核接口和对外输出；
- `tracked_idx` 是被选中在线 CPU 集合中的连续内部下标。

`src/core/cpu_inventory.c` 统一拥有 present、online、tracked membership。
`for_each_tracked_cpu` 是常规内部遍历接口。`--cpu` 是采样过滤器，因此未选中的
CPU 不会占用 per-CPU PMU 或 sysfs 资源。当前编译表示范围为 CPU ID
`0..1023`；更多 CPU 或更高编号会明确报告为截断。

在线 CPU membership 改变时，运行时重建所有 CPU 相关缓存与拓扑、重置
aggregator，并在恢复输出前重新消费一次 baseline。重建区间不伪装成正常测量行。

### 三层采样

- Static/rebuild：CPU membership、拓扑、传感器路径、cpuidle 状态身份、PMU 元数据。
- Slow-changing：频率上下限、governor、boost、cpuidle enable 状态，使用滚动预算刷新。
- Per-interval 按需采集：当前频率、Busy/Idle 输入、split cpuidle 计数器、
  package 功耗、温度、内存带宽和 PMU 都只在所选字段需要时读取；
  `/proc/stat` 由被选中的 Busy/Idle、LPI residency 与系统计数消费者共享。
  cpuidle 还会按输出 scope、state index 和 counter 类型精确选择：summary
  residency 不读取 `stateN/usage`；只导出 CPU `LPI-N_usage` 时不读取
  `stateN/time` 或 `/proc/stat`。

`src/core/sample_cache.c` 管理复用存储与缓存描述符。`src/core/collector.c` 为多个
下游消费者共同使用的快照字段提供 accessor；当前快照结构还没有完全 opaque。
记录构造器同样会跳过当前输出策略不可能发出的 per-CPU 与 package scope 物化。

## 指标不变量与来源

README 保存完整的用户字段说明。实现与序列化必须维持以下不变量：

- 每个有效 CPU 区间满足 `Idle% + Busy% = 100%`。
- `IOWait%` 是独立的 `/proc/stat` 视角，不从 `Busy%` 中扣除。
- `--busy-source=auto` 下，普通 CPU 使用 `/proc/stat`；识别为 `nohz_full`
  的 CPU 在输入有效时使用 `/proc/schedstat` runtime。
- `LPI-*` 是 authoritative idle time 的显示分解。最深的可见可用状态吸收残差，
  使显示状态之和接近 `Idle%`；原始 cpuidle 计数器不是第二套 Busy/Idle 权威值。
- per-CPU `LPI-N_usage` 是有限、非负的 `stateN/usage` 区间 delta 除以实测
  interval 秒数；读取失败或计数器回退时当前样本不可用，恢复后重新建 baseline。
- `SUM` 百分比与频率是有效 tracked CPU 的平均；系统计数器与 PMU 是区间 delta
  或聚合值。
- Package 行按 topology 提供的 physical package ID 聚合 tracked CPU。
- `Freq` 是本轮当前 `cpuinfo_cur_freq` 采样值；summary 与 package 值是当前
  CPU 采样值的跨 CPU 均值，不是时间平均，也不是硬件计数器推导的有效频率。
- Energy 由区间平均 package 功耗与实测时长推导。
- 只有命名为 `cycles` 与 `instructions` 的 delta 都有效且 cycles 非零时才输出 IPC。

主要内核来源如下：

| 指标 | 来源 | 输出单位 |
|---|---|---|
| 当前/最小/最大频率、governor、boost | CPU `cpufreq` sysfs | MHz/string/bool |
| Busy/Idle 与 IOWait | `/proc/stat`，可选 `/proc/schedstat` | % |
| split idle 与 usage rate | CPU `cpuidle` sysfs | %, /s |
| package 功耗 | 唯一且明确的 `power_meter`/`power1_average` 来源 | mW |
| 区间能量 | 由功耗与时长推导 | J |
| 温度 | 选定的 `thermal_zone` 策略 | degC |
| 内存带宽 | 唯一且明确的平台计数器 | MiB/s |
| 上下文切换与中断 | `/proc/stat` | count/interval |
| PMU 与 IPC | `perf_event_open()` | count/interval, instructions/cycle |

功耗与内存带宽发现要求恰好一个匹配来源。存在歧义时保持 unavailable，并由
`--probe` 解释；实现不会依赖目录遍历顺序随意选择。PMU 通常要求 root 或宽松的
`perf_event_paranoid`。

Unavailable 必须贯穿整个数据链保持 unavailable。有效的零是数据，不能拿来替代
读取失败、重新建立 baseline 或平台不支持。

## 输出契约

当前机器输出使用 `schema_version = 8`。`armstat --list` 是当前精确字段 ID、
scope、类型、单位、text label 与 JSON key 的权威查询；它与序列化器读取同一份
field registry。

### 稳定区间元数据

每个 JSON interval 和每行 CSV 都包含等价的起始元数据：

| 字段 | 含义 |
|---|---|
| `schema_version` | 整数兼容性门槛 |
| `interval` | 从 1 开始的可见区间编号 |
| `duration_us` | monotonic clock 实测采样窗口，单位微秒 |
| `timestamp` | Unix wall time 整秒 |
| `timestamp_ns` | Unix wall time 纳秒；程序处理首选时间轴 |
| `timestamp_iso` | 带 9 位小数的 RFC 3339 本地时间 |

三个 timestamp 表示同一个样本结束时刻；`duration_us` 表示测量窗口，不是 wall
clock 的显示精度。

### JSON

JSON 是顶层数组，每个可见区间对应一个对象。对象始终含上述元数据，还可能含：

- `summary`：已启用的 system-scope 字段与可选汇总 `pmu` 对象；
- `packages`：每个 package 一个对象，只用 `package` 标识一次；
- `cpus`：每个 tracked CPU 一个对象，用真实 Linux `cpu` 标识。

Section 是否存在由层级选择决定。默认输出 `summary` 和 `packages`；`-S` 只输出
`summary`；`-a` 再展开 `cpus`。显式选择可以输出任意有效组合。

人类可读的多行文本会用 `--- interval N ---` 标记每个采样块，并在连续采样块
之间留一个空行。该标记不进入 JSON 或 CSV；summary-only 文本仍是每个 interval
一行，quiet 文本则和其他人类可读表头一起省略该标记。

不可用的数字和字符串输出 JSON `null`。可用 Boost 输出 JSON boolean。内部非有限
浮点数会被归一化成 `null`，不会生成非标准 `NaN` 或 infinity token。字符串经过
JSON escaping。

### CSV

Header 只打印一次。稳定元数据之后有四种布局：

- summary-only：身份列为 `Scope`，值为 `SUM`；
- package-only：身份列为 `Package`；
- CPU-only：身份列为 `CPU`，值为真实 Linux CPU ID；
- mixed-scope：身份列为 `Scope,CPU,Package`，每个 interval 内按 `SUM`、`PKG`、
  `CPU` 顺序输出。

Compact header 使用 JSON 的标准字段 key。Mixed header 额外使用
`summary.<field>`、`package.<field>`、`cpu.<field>` 限定字段；compact PMU 字段为
`pmu.<event>`，mixed PMU 字段为 `summary.pmu.<event>` 与 `cpu.pmu.<event>`。
不属于当前行 scope 的单元格和 unavailable 值为空；有效零保持为零。含逗号、
双引号、换行或回车的字段按 CSV 规则加引号，并把内部双引号写成两个双引号。

不要手工按逗号切分 CSV；应使用标准 CSV parser，并通过身份列筛选行。

### 单位与兼容性

标准单位为 MHz、百分比、degC、mW、J、MiB/s、usage delta/s、count/interval 与
instructions/cycle。显示小数位是呈现策略，不等同于传感器精度声明。

消费者必须要求 `schema_version = 8`；项目尚未上线，不保留更早机器契约兼容。
消费者不得把缺失值转成零。

## 导出画图

可选脚本需要 Python 3 与 matplotlib：

```bash
python3 -m pip install matplotlib
```

生成 summary 或 CPU 输入：

```bash
armstat -S -f json -O summary.json
armstat -S -f csv -O summary.csv
armstat -a -f json -O cpus.json
armstat -a -f csv -O cpus.csv
```

安装后使用 `armstat-plot-summary` 处理 summary 序列，使用
`armstat-plot-cpu` 处理 per-CPU 与分组序列；源码树中的等价入口分别是
`scripts/plot_sum.py` 与 `scripts/plot_cpu.py`。两者共同使用共享 loader 进行
schema 校验、字段别名、缺失值和时间戳处理。输入必须含脚本支持的整数
`schema_version`；缺失或小数版本会被明确拒绝，不做猜测。

```bash
armstat-plot-summary summary.json --preset freq
armstat-plot-summary summary.json --y power --y2 temp0
armstat-plot-cpu cpus.json --preset busy --top 8
armstat-plot-cpu cpus.csv --group-by node --y busy
armstat-plot-cpu cpus.csv --y lpi0_usage --top 8
```

用 `--list-fields` 查询可画字段。大型服务器上的 CPU 图通常应使用
`--cpu-filter`、`--top` 或 `--group-by`。JSON 会完整载入内存；CSV 配合
`--sample-range` 时只保留请求的样本窗口。缺失值转换为 `NaN`，图中表现为断点；
preset 所需数据全部不可用时会明确报错，不生成空图。只有所选样本的时间戳全部有效
时才使用真实时间；否则整个横轴统一回退为样本号，避免混用两种坐标类型。
横轴保留导出记录携带的 RFC 3339 时区偏移，并把偏移写入轴标签，因此把导出文件
移动到另一个时区的机器上画图不会改变显示时钟。如果一个时间窗内的偏移发生变化，
横轴统一转为 UTC；wall-clock 时间不递增时则回退为样本号，避免折线在横轴上倒退。
字段列表和坐标轴会显示已知字段的标准输出单位。平滑按样本计算，并在当前样本
不可用时保留断点，因此采集失败或 CPU 下线不会被画成沿用旧值。在所选时间窗内
完全没有主字段数据的 CPU 或分组会被报告并跳过；间歇缺失仍然显示为断点。
对于双轴 CPU 图，只缺次字段的实体仍保留主线，其空的次轴线会被报告并跳过。
`--group-by core` 使用
`(package, core)` 身份，不会把不同 package 中编号相同的 core 合并。
`--rank-by avg` 是可见样本的算术平均，不是按采样时长加权的时间平均。
summary 图使用 10 种不同颜色，足以覆盖完整 `idle-lpi` preset 的全部曲线而不复用
颜色。两个命令都强制使用无界面渲染后端，并先在输出目录写临时文件；只有完整的
PNG、SVG 或 PDF 生成成功后才原子替换目标文件。

## 构建与验证

常规开发门槛：

```bash
make clean
make
make debug-test
make analyze
```

`make test` 覆盖 C 计算与策略、CLI/错误路径、text/JSON/CSV 契约、streaming、
plot loader、安装 matplotlib 时的真实渲染，以及构建/安装切换。设置
`ARMSTAT_REQUIRE_PLOT_RENDER=1` 后，缺少 matplotlib 会使测试失败；CI 安装该依赖
并启用此门槛。`make debug-test`
使用 AddressSanitizer 与 UndefinedBehaviorSanitizer 重建并运行完整测试。在
Linux + GCC 上，`make analyze` 把 path-sensitive static analyzer 结果写入
`.armstat-analysis/`。

Out-of-tree 构建通过 `O` 指定，所有生成文件都应留在该目录：

```bash
make O=/tmp/armstat-build
/tmp/armstat-build/armstat --version
```

### ARM64 目标机验收

主机无关测试不能证明硬件行为。应在每个声明支持的鲲鹏 ARM64 Linux 服务器型号上
先检查能力，再运行：

```bash
./armstat --probe
make target-test
```

目标测试覆盖基础选项、输出失败、probe 结构、默认/summary/JSON/CSV 执行、时间戳、
缺失值、进程资源，以及显式要求的 optional telemetry。能力门槛可这样启用：

```bash
ARMSTAT_REQUIRE_PMU=1 \
ARMSTAT_REQUIRE_CPUIDLE=1 \
ARMSTAT_REQUIRE_POWER=1 \
ARMSTAT_REQUIRE_TEMP=1 \
ARMSTAT_REQUIRE_MEMBW=1 \
ARMSTAT_REQUIRE_UNCORE=1 \
make target-test
```

只启用部署平台明确承诺的能力。稳定性运行使用 `ARMSTAT_SOAK_ITERATIONS` 与
`ARMSTAT_SOAK_INTERVAL`；资源上限可通过 `ARMSTAT_MAX_RSS_KIB`、
`ARMSTAT_MAX_OPEN_FDS` 和 `ARMSTAT_MAX_DIAGNOSTIC_LINES` 配置。

启用 `ARMSTAT_REQUIRE_CPUIDLE=1` 后，目标门槛会验证每个可见的
`idle_state_N_name` probe 映射、汇总驻留率，以及至少一个有限且非负的 per-CPU
`lpi0_usage` 样本。该样本还会通过 CPU 画图加载器，因此验收链路覆盖采集、导出和
画图输入，而不是只停留在文本显示。

发布前必须通过所有主机门槛、符合平台能力的 target test，检查默认 text、`-S`、
JSON、CSV 样本，并在真实服务器持续运行。容器或虚拟 ARM64 可以证明可移植性与输出
契约，但不能证明真实 PMU、cpuidle、功耗、温度或内存带宽语义。

## 安装

`make install` 安装：

- `armstat`、`armstat-plot-summary` 与 `armstat-plot-cpu` 到 `PREFIX/bin`；
- `man/armstat.8` 到 `PREFIX/share/man/man8`；
- README 中英文到 `PREFIX/share/doc/armstat`，本参考中英文到
  `PREFIX/share/doc/armstat/docs`；
- 共享画图模块到 `PREFIX/share/armstat`。

默认 `PREFIX` 为 `/usr`，支持用 `DESTDIR` staging。

## 文档维护

用户可见行为按以下顺序同步：

```text
code -> README.md -> README.zh-CN.md -> man/armstat.8
     -> docs/REFERENCE.md -> docs/REFERENCE.zh-CN.md -> tests
```

优先更新现有章节，不要为每个主题新建 Markdown。只有读者、生命周期或安装位置与
README、man page、本文都确实不同时，才新增文档。
