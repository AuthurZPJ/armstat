# armstat

<p align="center">
  <a href="README.md">English</a> | <b>简体中文</b>
</p>

`armstat` 是面向 ARM64 Linux 服务器的监控工具，风格接近 `turbostat`。
它观察 CPU 频率、Busy/Idle、空闲状态、功耗与能量、温度、PMU 计数和 IPC，
通过 Linux 的 `sysfs`、`procfs`、`hwmon` 与 `perf_event_open()` 采集数据，
输出带时间戳的文本、JSON 或 CSV，并提供绘图工具。

首发目标平台为 **华为鲲鹏**。其他 ARM64 系统也可能提供兼容的基础指标。
每个鲲鹏型号在部署前都需要通过[目标机验收](docs/REFERENCE.zh-CN.md#arm64-目标机验收)；
容器测试不能单独证明硬件支持。

## 快速开始

在 ARM64 Linux 上使用 C 编译器和 `make` 构建，运行时需挂载 `/proc` 与 `/sys`：

```bash
make
./armstat --probe
./armstat -i 1 -n 5
./armstat -a -i 1 -n 5
./armstat -S -a -f json -o summary.json -i 1 -n 5
sudo make install
```

`--probe` 列出可用数据源。PMU/IPC 通常需要 root 或宽松的
`/proc/sys/kernel/perf_event_paranoid` 设置。可选指标是否可用，取决于服务器
提供的接口及传感器映射。

安装后可使用 `armstat`、`armstat-plot-summary`、`armstat-plot-cpu` 和
`man armstat`。绘图另需 Python 3 与 matplotlib，采集器本身不依赖 Python。
交叉编译见[构建参考](docs/REFERENCE.zh-CN.md#构建与验证)，分阶段安装见
[安装说明](docs/REFERENCE.zh-CN.md#安装)。

## 如何读输出

| 模式 | 每个采样区间的行 |
|---|---|
| 默认 | 一行 `SUM` |
| `-a` | `SUM` 与每 CPU 动态指标，保留 CPU 拓扑 |
| `-S` | 一行 `SUM` |
| `-s cpu` | 每 CPU 频率、Busy/Idle/IOWait、LPI 驻留、温度与可用 IPC |
| `-s all` | 完整基础视图，包含插槽行、配置与 LPI usage |
| 显式 `-s` | 所选列组或字段要求的层级 |

周期采样的第一条结果在一个完整区间之后输出。`-n 1` 测量一个区间后退出，
`-n 0` 持续运行到收到中断。默认区间为一秒，接受 `0.000001`–`31536000` 秒。比率公式使用
实测窗口，它可能与请求的区间时长不同。

只输出 `SUM` 时，文本每个区间输出一行带时间的数据；其他视图按区间分块，
标明时间与实测时长。文本时间保留日期和时区，小数精简为毫秒。
表头带单位，例如 `CtxSw[/int]` 表示本区间内的计数。`-q` 只隐藏启动说明；
`--quiet-all` 还省略表头、时间和区间标记。能取得有效尺寸的交互式终端默认按
屏幕高度自动重复表头；
`-N N` 改为每 N 个样本重复，`-N 0` 关闭自动重复。重定向、管道或无法取得有效
尺寸的终端，默认只在启动和布局变化时打印表头，也可用 `-N N` 指定重复间隔。

`SUM` 中的字段按各自规则汇总：

| 字段类别 | 含义 |
|---|---|
| 频率、Busy/Idle | 所选 CPU 有效值的平均 |
| LPI 状态 | 所选 CPU 对应状态的平均，要求每个 CPU 的值都有效 |
| PMU | 所选 CPU 的区间计数之和，要求所有 CPU 事件组都有效 |
| 系统计数器 | 全局区间计数 |
| 功耗、温度、内存带宽 | 平台数据源，各自保留传感器覆盖范围 |

插槽（package）行默认隐藏；使用 `-s pkg`（或 `package`）、`-s pkg_freq_mhz` 等
精确字段或 `-s all` 按需显示。它按物理 package ID 汇总所选 CPU。CPU 行使用
真实 Linux CPU 编号，按编号排序；package 拓扑仍可用于 CPU 身份识别与绘图分组。

### 主要指标

| 字段 | 单位 | 解读 |
|---|---|---|
| `Freq`、`Min`、`Max` | MHz | 当前频率采样值与配置上下限 |
| `Idle%`、`Busy%`、`IOWait%` | % | 区间利用率与 I/O 等待记账 |
| `LPI-N` | % | 状态 N 的 cpuidle 时间增量除以区间时长 |
| `LPI-N_usage` | /s | 每 CPU 状态进入次数的区间增量/秒 |
| `Power`、`Energy` | mW、J | package 传感器功耗与区间能量估算 |
| `Temp`、`Temp0` ... `Temp3` | °C | NUMA/裸片温度映射 |
| `MemBW` | MiB/s | 平台内存读取字节计数器的速率 |
| `CtxSw`、`IRQs`、`SoftIRQs`、PMU | count/interval | 区间计数，不是每秒速率 |
| `IPC` | instructions/cycle | 指令数除以周期数 |

`Freq` 是瞬时采样值；`SUM` 和插槽行也只是当前 CPU 样本的平均，不是区间平均
频率或忙碌时的有效频率。显示小数位不代表传感器精度。完整的
[来源与公式](docs/REFERENCE.zh-CN.md#指标来源与计算)集中在综合参考中。

### Busy/Idle 与 LPI

默认的 `--busy-source auto` 对所有 CPU 使用 `/proc/stat`，包括 `nohz_full`
CPU。该路径将 iowait 包含在 `Idle%` 内；`Busy% = 100 - Idle%`，因此
`IOWait%` 是补充拆分，不是第三个互斥利用率分桶。短区间会受到 `USER_HZ`
记账精度影响，解读利用率时建议使用约一秒的区间。

`--busy-source schedstat` 用于诊断，不可用时按 CPU 回退到 procstat。任务运行
时间在上下文切换时结算，当前仍在执行的时间可能未计入该区间。`task-clock`
是这一实现的兼容别名。

每个 LPI 状态直接报告自己的 sysfs 区间统计，独立于 `Idle%` 和其他可见列。
例如 `Idle%=90`、`LPI-0=10`、`LPI-1=50` 无需补齐。隐藏某个状态不会重新
分配其时间，默认也不新增 `IdleGap` 列。

cpuidle 在状态退出时结算时间。长时间空闲可能使前几个窗口显示 0，退出后的
窗口超过 100%；工具保留这些值。软件记账不能证明硬件实际进入了请求的状态。
只暴露状态 0–7，更深状态不折叠到其他列；`--probe` 给出各 `LPI-N` 的内核名称。

状态缺失、禁用、读取失败或计数器回退时，对应值独立显示不可用。累计计数器
恢复时需重新建立基线，下一完整区间才能恢复统计。不可用值在文本中为 `-`，
JSON 中为 `null`，CSV 中为空。没有 cpuidle 支持时，LPI 列隐藏，Busy/Idle
仍可使用。若初始化后所选视图只剩 CPU 行标识而没有数据列，启动报错。

## 常用命令

```bash
armstat -n 1                           # 测量一个完整区间
armstat -D                              # 立即读取原始 proc/sysfs 快照
armstat -q -- sleep 2                    # 围绕命令执行测量一个区间
armstat -S -a -i 1 -n 10                # 扩展汇总字段
armstat -a -c 0,1,4-7                   # 选择 CPU
armstat -s cpu                         # 常用 CPU 指标，不带 SUM
armstat -s cpu,freq,idle                # 指定指标的 SUM 与 CPU 行
armstat -s freq-config                 # CPU 频率上下限、governor、boost
armstat -s idle-usage                  # 每 CPU 空闲状态进入次数/秒
armstat -s all                         # 完整基础视图
armstat -s all,membw                   # 额外请求内存读取带宽
armstat -s pkg_freq_mhz                 # 仅插槽频率
armstat -s power,temp                   # 功耗与温度
armstat -H temp                         # 隐藏温度
armstat -H ipc                          # 不自动采集 IPC
armstat -P                             # 禁用 perf PMU 采集
armstat -s ipc                          # 仅汇总 IPC，明确报告采集问题
armstat -I                             # 周期、指令与 IPC
armstat -J                             # 区间能量
```

`--cpu` 过滤采样，包括 PMU 资源和由 CPU 数据计算的汇总值；平台传感器和全局
计数器仍保留各自范围。默认汇总基于所选 CPU，展开 CPU 行后仍保留，例如
`-a -c 0-3`；插槽行继续按显式选择输出。`-s cpu` 仍仅输出 CPU 行；只需过滤后
的汇总时使用 `-S --cpu ...`。

`freq` 选择当前频率与 UncoreFreq，`idle` 选择 LPI 驻留及 Busy/Idle/IOWait。
`freq-config`、`idle-usage` 分别选择 CPU 配置和状态进入速率。`-a` 省略这两组
及插槽行，CPU 的 `Pkg`、`Core`、`Node` 列仍用于身份识别与分组。
`MemBW` 需要显式选择：使用 `-s membw`（或 `mem`）、`-s mem_bw`，可单独请求或
与其他选择组合。默认、`-S`、`-a` 和 `-s all` 均不会自动启用它。

列组名不区分大小写，`Freq` 与 `freq` 相同。匹配顺序为列组、精确稳定字段 ID、
文本/JSON 别名。重名别名不会展开行层级；通过 `cpu`、`core`、`numa` 或稳定
CPU 字段 ID 请求 CPU 行；仅属 CPU 的别名，如 `Min`、`lpi0_usage`，也会自动展开。
`core`、`numa`、`freq-config`、`idle-usage` 自动启用
CPU 行。单独 `-s cpu` 提供常用动态指标且不带 SUM，组合选择按请求的字段与
层级输出；快捷默认不会重新启用显式 `-H` 排除的字段。独立 `-p`、`-I`、`-J`
请求保留所需的 SUM 输出。

首个 `-s` 替换之前的视图设置：`-a -s cpu` 使用 CPU 快捷视图，`-s cpu -a`
使用扩展动态视图。

`-s` 与 `-H` 接受列组或字段。首个 `-s` 建立白名单，并保留显式 `-p`、`-I`、
`-J` 请求；重复 `-s` 取并集，包括 `-s all` 的完整选择，不会重新启用被 `-H`
隐藏的无关列组。
未知字段、非法 CPU 列表、匹配不到在线
CPU，以及不能输出任何行的选择都会在启动时失败。

`armstat --list` 列出字段 ID、层级、单位、文本标签、JSON key、列组和 PMU
名称。脚本中优先使用稳定字段 ID。`armstat --help` 列出完整选项。

### 命令模式与原始快照

`armstat [选项] -- command [参数...]` 围绕命令执行测量一个区间，包含少量采集、
启动和等待开销。统计覆盖所选 CPU 和平台来源，不限于该命令的进程。默认将
统计写到 stderr，命令保留原有
stdout 和 stderr；JSON/CSV 建议用 `-o` 写文件，避免混入命令的 stderr。
命令模式不能与显式 `-i`、`-n`、`-D`、`--probe` 或 `-l` 组合。

初始化失败时不执行命令。测量成功后，即使命令失败也输出统计，并返回命令的
退出码：找不到命令为 127，其他 exec 失败为 126，被信号终止为 128 加信号编号。
监控或输出失败返回 1。INT、TERM、HUP、QUIT 转发给命令的进程组，保留前台
终端访问和 shell 作业控制。

`-D`/`--Dump` 不等待区间，立即输出一份诊断文本，包含累计计数器和当前传感器
读数并标明单位，不计算增量或利用率。`-c` 限定 CPU，视图和字段选择不限制快照。
它不能与命令、显式 `-i`/`-n`、`--probe`、`-l`、非文本格式或显式 PMU/IPC
请求组合；`-q` 和 `-N` 不改变诊断正文。

### 从 turbostat 迁移

规范选项为 `-n`/`--num_iterations`、`-N`/`--header_iterations`、
`-S`/`--Summary`、`-J`/`--Joules`、`-o`/`--out` 和 `-D`/`--Dump`。
长选项区分大小写，旧拼写和 `-O` 不再接受。`-P`/`--no-perf` 禁用 PMU 采集
及 `--probe` 的 perf 测试，与显式 PMU/IPC 请求冲突；探测只用 `--probe`。
Busy/Idle 来源选择规则保持不变。

armstat 保留自己的列组、格式和默认一秒区间，并非完全兼容的替代品。相近字段
应按以下口径解读：

| turbostat 概念 | armstat 含义 |
|---|---|
| `Avg_MHz` / `Bzy_MHz` | `Freq` 是当前 sysfs 采样值，不是区间平均或忙时频率 |
| 硬件 C0 `Busy%` | 默认 `Busy%` 来自软件 `/proc/stat` 记账 |
| 空闲驻留 | `LPI-N` 是独立 cpuidle 区间统计，合计无需等于 `Idle%` |
| 功率 W / 能量 J | `Power` 使用 mW；`-J` 在功耗旁增加估算的区间 `Energy[J]` |

### PMU

```bash
armstat -p cycles,instructions
armstat -p cache-misses,branches
armstat -p 0x11
```

默认、`-S`、`-a`、`-s all` 和 `-s cpu` 视图尝试用 `cycles,instructions` 提供
IPC，不增加原始计数列或 CPU 行。所选 N 个 CPU 需要 2N 个 perf 文件描述符。
任一 CPU 事件组初始化失败时，隐藏自动 IPC 并关闭其计数器；除 `-d` 外不报
诊断。`-H ipc` 关闭这项自动采集。其他 `-s` 白名单只有明确选择 IPC 才采集它。

`-p` 选择原始事件计数并抑制自动 IPC；需要 IPC 时加 `-I` 或显式 IPC 选择。
`-I` 显示原始 `cycles,instructions` 及 IPC，`-s ipc` 或 `-s cpu_ipc` 只请求 IPC。
显式 IPC 请求会为自定义事件列表补齐所需事件对，并在初始化失败时报告问题、
保留不可用列。自动初始化成功后，后续读取失效也保留 IPC 列，以 `-`、`null`
或空单元格表示，保持 CSV 布局。

原始事件要求 `0x` 或 `0X` 前缀，后跟不超过 64 位的十六进制数。未知或重复
名称、超过 16 个事件的列表会被拒绝。已知事件打不开时仍显示为不可用。
`mem-read` 与 `mem-write` 统计退休的加载和存储指令次数，不是字节数。
可选事件依赖具体 CPU；事件组超过硬件或文件描述符容量时，应减少 CPU 或事件数。

## 导出与绘图

```bash
armstat -S -a -f json -o summary.json -i 1 -n 60
armstat -a -f csv -o cpus.csv -i 1 -n 60
armstat-plot-summary summary.json --preset freq
armstat-plot-cpu cpus.csv --preset busy --top 8
armstat-plot-cpu cpus.csv --group-by node --y busy
```

`-o`/`--out` 将统计写入文件。JSON 是区间对象数组，CSV 按选择输出汇总、插槽、CPU
或混合行。两者都含 `schema_version=9`、区间编号、实测时长和时间戳。随附
绘图加载器只接受第 9 版。
默认 JSON 只包含 `summary`，默认 CSV 使用 `Scope=SUM` 的汇总布局；显式选择
插槽字段或使用 `-s all` 仍可输出插槽行。

CSV 要求列布局固定。运行中能力变化导致列改变时，采集会在写入该区间前停止，
并返回非零状态；此时重新开始 CSV 采集，或使用允许字段集变化的 JSON。
已有输出文件只在初始化和基线采集成功后打开；后续失败保留已经写入的前缀。

绘图保留时间戳和不可用数据造成的空缺。使用输入文件的 `--list-fields` 查询
可用曲线。JSON 和 CSV 的 `--sample-range START:END` 都只保留所选样本，
`--cpu-filter` 还可减少保留的 CPU 曲线。大图会减少渲染点，保留分桶峰谷和缺失
区间边界；排名和平滑仍使用全部所选样本。从源码运行时，以
`scripts/plot_sum.py`、`scripts/plot_cpu.py` 替代
安装后的命令。详细规则见[绘图参考](docs/REFERENCE.zh-CN.md#导出画图)和
[导出契约](docs/REFERENCE.zh-CN.md#输出契约)。

排查采样开销时，`-d` 在 stderr 报告采集、聚合、输出耗时及跳过的采样时点数。
终端或管道读取过慢会延迟采样，`duration_us` 始终记录实测区间。

## 平台限制与排查

功耗要求恰好一个 `power_meter/power1_average` 数据源，内存带宽要求恰好一个
`mem_bytes_read` 数据源。发现存在歧义时，指标保持不可用，`--probe` 会说明原因。

默认温度策略将 `thermal_zoneN/temp` 映射为 `TempN` 与 NUMA/裸片 N；CPU 行的
`Temp` 使用该 CPU 所属 NUMA 节点的温度。映射不适合服务器时，设置
`ARMSTAT_TEMP_POLICY=none` 关闭它；其他合法值为 `thermal-zone-index` 与
`disabled`，未知值会导致启动失败。此后端不提供逐核功耗或逐核温度。

编译表示范围不含 1023 以上的 CPU ID；工具会告警并采样可表示的在线 CPU。
当前没有专门的核心聚合行。CPU 热插拔会重建相关状态，在重新建立基线后恢复输出。

数据缺失或异常时，先检查：

```bash
armstat --probe
armstat --list
armstat -d -i 1 -n 2
```

`--probe` 报告数据源路径、候选数量、CPU 拓扑、Busy/Idle 策略，以及
`probe_schema_version: 1`。PMU 探测只在首个所选 CPU 上打开 `cycles`，每个事件
能否在每个 CPU 上使用，仍需通过实际采集确认。`--probe` 的发现范围不受
`-s`/`-H` 选择影响。

## 开发与文档

```bash
make test
make debug-test
make analyze
make target-test    # ARM64 Linux 运行检查
```

- [综合参考](docs/REFERENCE.zh-CN.md)：架构、公式、导出、绘图、构建、安装与目标机验收。
- `man armstat`：安装后的命令行手册。
- 源码目录中的 `AGENTS.md`：代码与文档维护规则。

项目采用 GPL-2.0 许可证，完整文本见 [COPYING](COPYING)。
