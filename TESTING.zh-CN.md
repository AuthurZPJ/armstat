# armstat 测试说明

<p align="center">
  <a href="README.zh-CN.md">← 返回 README</a> |
  <a href="DESIGN.zh-CN.md">设计</a> |
  <a href="EXPORTS.zh-CN.md">导出</a>
</p>

本文档说明当前 `armstat` 的测试方式。

## 目标

当前测试策略分成两层：

- 任意开发机上都能快速执行的本地回归检查
- 需要在目标 ARM 服务器上执行的实机验证

第一层主要保护代码结构和机器可读导出契约。  
第二层主要验证 cpuidle、PMU、NUMA 温度映射、package 功耗等平台相关行为。

仓库 CI（`.github/workflows/ci.yml`）会在 push 与 pull request 时使用 Ubuntu
24.04 ARM64 runner：将 GCC warning 提升为错误，包括 format、未定义宏、
shadowing 与缺失/非严格 prototype 检查；运行 GCC 静态分析器和
AddressSanitizer/UBSan 套件；切回默认 fortified release flags 做最终复测；
最后执行 `make target-test` 运行态验收和 1,000 次短周期资源稳定性 soak。
PMU 与传感器的硬件特定语义仍必须完成下述真实服务器层验证。

## 1. 本地构建检查

在仓库根目录执行：

```bash
make clean
make
```

这一步主要确认：

- C 源码仍能在 format、未定义宏、shadowing、严格/缺失 prototype 告警开启时
  正常构建
- formatter / export 路径仍然可以正确链接
- helper scripts 与当前源码树仍然兼容

如果要做内存或未定义行为排查，也可以用 sanitizer 版本：

```bash
make debug-test
```

这个目标会用 AddressSanitizer 和 UBSan 重新构建 binary 与全部 C 测试，
然后运行完整套件。只需要带检测器的 binary 时可用 `make debug`。

Linux + GCC 环境还可以运行路径敏感的编译器静态分析：

```bash
make analyze
```

该目标会把 analyzer 发现视为错误；唯一屏蔽的是 armstat 有意保留到进程结束、
并在统一清理阶段关闭的 `/proc` stream cache 所触发的 file/allocation leak 误报。
Analyzer object 与 binary 都保留在 `.armstat-analysis/`，因此这项检查不会替换
已经可以安装或运行的 release binary。

外部构建必须把所有生成文件留在指定输出目录：

```bash
make O=/tmp/armstat-build
/tmp/armstat-build/armstat --version
```

构建目录会记录 compiler identity、compiler target 与 flags；compiler、version、
target、`CFLAGS`、`LDFLAGS` 或 `LDLIBS` 变化时，会使不兼容 object 失效，而
不是静默复用。构建回归中的每一次 release/custom/release 切换都使用并行构建，
并比较 binary checksum，同时覆盖粗粒度文件时间戳和普通增量构建场景。
该回归还会确认测试程序在 flags 改变后重新链接、`VERSION` 能进入构建元数据与
`--version`，并在 Linux 上检查默认可执行文件具有 PIE、RELRO 与立即绑定。
建立 release baseline 前会清除继承的 compiler、linker、cross-build 与输出目录
覆盖项，因此父级测试即使使用 sanitizer 或 coverage flags，也不会意外削弱检查。

## 2. Smoke Test

执行：

```bash
make test
```

当前会运行：

- `tests/test_core_logic`
- `tests/test_column_selection`
- `tests/test_runtime_smoke`
- `tests/test_cpu_inventory`
- `tests/test_section_policy`
- `tests/test_cli_smoke.sh`
- `tests/test_plot_loaders.py`
- `tests/test_csv_streaming.py`
- `tests/test_build.sh`

这些 smoke test 保持得很小、很快，主要验证：

- idle 百分比、busy 百分比、iowait 属于 idle 的记账方式、schedstat clamp
  和 procstat jiffy 换算溢出处理正确
- schedstat CPU 记录必须有 9 个字段，并使用内核文档中的第 7 字段
- 严格数值与 CPU list parser 会拒绝 malformed、溢出、反向区间和空 token，
  以及被截断的输入，同时接受很长的稀疏列表，并保留固定 CPU mask 之外的
  真实总数
- ARM PMUv3 内置别名会解析成预期的架构事件号；重复 PMU 事件名和超限列表会
  分别被拒绝；只有两个命名输入都存在时 IPC 才可用
- PMU 初始化会针对 tracked CPU/事件需求尽力提高受限的 soft
  `RLIMIT_NOFILE`，但不会超过 hard limit
- procstat、功耗、内存带宽、系统计数和 PMU 瞬时失败时输出不可用值，恢复后
  为累计型来源重新建立 baseline
- 频率、稀疏/带符号温度、split-idle 和 PMU 的有效性会一直传到
  text/JSON/CSV，不会伪造零值
- package 聚合不会在第 16 个不同 package ID 后静默停止
- online-mask 匹配会同时检查具体 CPU 成员、可表示数量和真实总数，因此 hotplug
  fast path 不会漏掉总数相同的成员替换或 CPU ID 1023 之外的变化
- hotplug 重建会刷新 tracked cpufreq 状态，并在恢复 interval 输出前消费新
  baseline
- `-s` / `-H` 的精确字段选择不会误伤整组列
- 重复 `-s` 取并集，`-p` / `-I` / `-J` 写在 show whitelist 前后都保持生效
- `-a` / `all` 只会打开基础列组，不会隐式打开 PMU / IPC
- `-H all` 也会把显式启用过的 PMU / IPC 一起关闭
- `-S -a`、`-a -I`、`--probe`、busy-source 解析这些主流程组合仍然正常
- 标准 `-h` 能成功退出，静态 `--list` 不夹带平台探测噪声并从 registry 暴露
  精确 field ID 及类型/单位元数据，help 会说明无限运行与单个完整 interval
  的区别
- field ID 与 JSON 键在各自 serialization scope 内保持唯一
- 平台 probe 失败不会截断已有输出文件
- 非法 `ARMSTAT_TEMP_POLICY` 会被拒绝，且不会截断已有 probe 输出文件
- probe 会输出可解析的 package 功耗与内存带宽候选数量；发现有歧义时不会选择
  第一个匹配项
- Linux 报告输出写入错误时，`--help`、`--version`、`--list` 和 `--probe`
  会返回失败，而不是静默报告成功
- 下游管道关闭时会走受检输出失败并返回 1，而不是被 `SIGPIPE` 直接终止
- 启动 banner 对 1 秒、10 毫秒和 1 微秒 interval 均不会把合法亚秒值显示成零
- schema 7 导出包含正数的实测 `duration_us`、保留纳秒时间戳并使用 RFC 3339
  偏移；画图 loader 优先使用纳秒时间，亚秒样本不会坍缩到同一个整秒点
- 目标采集的样本时间严格递增，节拍来自 monotonic deadline，而非输出完成后再睡眠
- 绝对 deadline 算术会在下一个 slot 前保持相位、跳过一个或多个已过期 slot
  而不补采，并拒绝溢出
- `-D` text 默认保留自解释表头，只有显式 `-D -q` 才无表头
- JSON / CSV serializer 的 summary-only、package-only、CPU-only 与等宽
  `SUM`/`PKG`/`CPU` mixed-scope 输出仍然能端到端生成
- summary CSV 使用 `Scope` 表头与 `SUM` 行值，package 身份只序列化一次，
  不可用字符串保持缺失，JSON boost 为布尔值，interval 计数不带误导性小数
- 精确选择 package 字段会开启 package 输出，不会得到只有表头的 CSV
- 空的或与模式不兼容的有效选择会在平台发现后被拒绝，且不会先截断已有文件
- 成功的 `--output` 会用合法 JSON 替换旧内容，`--export` 能写出 probe，输出
  路径若是目录则会明确失败
- summary JSON 仍能被 `scripts/plot_sum.py` 读取
- summary CSV 仍能被 `scripts/plot_sum.py` 读取
- CPU JSON 仍能被 `scripts/plot_cpu.py` 读取
- CPU CSV 仍能被 `scripts/plot_cpu.py` 读取
- 10,000 行 summary 与 1,000 个八 CPU 样本的 CSV sample-range 读取保持
  streaming；JSON 继续采用文档约定的全量加载模型
- 当前机器可读导出符合 `schema_version = 7`，同时画图 loader 保留 version
  4 至 6 兼容性、按 scope 解析 `freq`，并在 SUM/CPU 图中忽略 package 行
- 空 JSON stream 仍是合法数组
- 外部构建、编译 flags 切换、staged install / uninstall 和许可证安装端到端正常

这些测试 **不会** 验证 ARM 平台上的运行时语义。

## 3. 目标 ARM 服务器实机验证

先在真实机器上执行自动化运行态验收：

```bash
make target-test
```

它会检查 probe 契约版本与 CPU 数、schema 7 JSON/CSV 采集、实测 interval
时长、RFC 3339 时间、summary/mixed CSV 行宽、真实 `SUM`/`PKG`/`CPU` 身份、
精确 package-only CSV 选择、必需功耗/内存带宽能力对应的 sysfs 源、来源候选
数量与唯一性、普通 `--output`/`--export` 文件行为，以及第一个
interval 前被中断时是否仍输出合法空 JSON。
可通过 `ARMSTAT_TARGET_INTERVAL` 与
`ARMSTAT_TARGET_SAMPLES` 调整短采集。

默认情况下，平台能力缺失会被当作显式降级配置接受，因此测试也能在通用 ARM64
CI 主机运行。对真实部署候选机，应把该平台承诺提供的能力分别设为 `1`：
`ARMSTAT_REQUIRE_PMU`、`ARMSTAT_REQUIRE_CPUIDLE`、
`ARMSTAT_REQUIRE_POWER`、`ARMSTAT_REQUIRE_TEMP`、
`ARMSTAT_REQUIRE_MEMBW` 和 `ARMSTAT_REQUIRE_UNCORE`。被要求的能力必须同时被
`--probe` 识别，并在普通 JSON 采样中产生有限数值；PMU 还必须至少产生一组大于
零的 `cycles` / `instructions` 以及有效 IPC。例如：

```bash
sudo env ARMSTAT_REQUIRE_PMU=1 ARMSTAT_REQUIRE_CPUIDLE=1 \
  ARMSTAT_REQUIRE_POWER=1 ARMSTAT_REQUIRE_TEMP=1 \
  ARMSTAT_REQUIRE_MEMBW=1 make target-test
```

只启用部署硬件契约实际承诺的能力。PMU 验收通常需要 root 或宽松的
`perf_event_paranoid` 设置。要在同一次运行中包含下述 30 分钟门槛，使用：

```bash
ARMSTAT_SOAK_ITERATIONS=1800 ARMSTAT_SOAK_INTERVAL=1 make target-test
```

soak 期间如果诊断超过 16 行、JSON 不合法或不完整、常驻内存超过 256 MiB，
或打开文件数超过 256，测试都会失败。数量有界的启动诊断仍会完整展示，以便
核对预期的能力降级。部署规格确有不同上限时，可用
`ARMSTAT_MAX_DIAGNOSTIC_LINES`、`ARMSTAT_MAX_RSS_KIB` 与
`ARMSTAT_MAX_OPEN_FDS` 明确覆盖。

下面这些命令仍是最值得在真实机器上做的手工检查。

### 基础输出

```bash
./armstat -i 1 -n 2
./armstat -i 1 -n 2 -S
./armstat -i 1 -n 2 -S -a
```

重点看：

- summary 行是否正常
- CPU 行是否按真实 CPU ID 排序
- 表头和数据是否对齐
- `Governor` / `Boost` / `Temp` / `LPI-*` 是否可读
- `--probe` 的 online/tracked 数是否符合主机；如果 CPU ID 超出可表示的
  `0..1023` 范围，应出现截断警告

### Idle / Busy 语义

```bash
./armstat -i 1 -n 3 -s cpu,LPI-0,LPI-1,Idle%,IOWait%,Busy%
./armstat -i 1 -n 3 -S --busy-source procstat
./armstat -i 1 -n 3 -S --busy-source schedstat
```

重点看：

- `Idle% + Busy%` 是否接近 `100`
- `IOWait%` 是否独立显示，但仍属于 procstat idle 的子集
- 可见 `LPI-*` 之和是否接近 `Idle%`
- 禁用/缺失 state 的唤醒字段是否显示不可用而非零
- 在 `nohz_full` CPU 上，`schedstat` 是否比 `procstat` 更稳定
- 来源瞬时读取失败后，相关字段是否在失败/恢复边界显示不可用，而不是产生
  0/100% 尖峰

### PMU / IPC

```bash
./armstat -i 1 -n 2 -I
./armstat -i 1 -n 2 -S -p cycles,instructions
```

重点看：

- 请求 PMU 时列是否正常出现
- PMU 不可用时是否清晰降级，而不是静默消失
- `-I` 是否不会破坏 Busy/Idle
- `--probe` 的 PMU 结果只代表第一个 tracked CPU 上的 `cycles` 检查；请求的
  完整事件集仍需通过正常采样验证
- 在稳定、绑核 workload 下与 `perf stat` 对比 cycles/instructions
- 验证 `mem-read` / `mem-write` 表现为 retired load/store 事件次数，而不是
  字节计数
- 使用足以触发 multiplex 的事件数量重复检查，确认 scaling 后的速率仍合理

### NUMA / 温度 / 功耗

```bash
./armstat -i 1 -n 2 -s cpu,pkg,core,node,temp
./armstat -i 1 -n 2 -S -s power,temp,energy
./armstat --probe
```

重点看：

- CPU 行是否映射到正确 NUMA 节点
- CPU 温度是否符合预期 `Temp0..` summary 温度映射
- 稀疏 thermal-zone 编号是否不会移动 `TempN` 身份；硬件若暴露负值，输出是否
  仍保持负数
- package 功耗是否只出现在 `SUM` 级
- `--probe` 是否反映真实平台传感器模型，并显示
  `summary_temp_policy`、候选数量、选中路径和歧义说明；生产指标不能依赖目录
  遍历顺序

### 导出验证

```bash
./armstat -S -f json -O summary.json -n 2
./armstat -S -f csv -O summary.csv -n 2
./armstat -f json -O cpus.json -n 2
./armstat -f csv -O cpus.csv -n 2
```

重点看：

- 导出里是否包含 `schema_version`
- 当前导出是否为 `schema_version = 7`
- JSON/CSV 是否包含正数的实测 `duration_us`
- CSV 是否包含 `timestamp`、`timestamp_ns` 和 `timestamp_iso`
- `timestamp_ns` 是否与 `timestamp` 一致，ISO 值是否为带小数秒和冒号分隔
  UTC 偏移的有效 RFC 3339
- summary CSV 是否使用 `Scope` 身份表头与 `SUM` 值
- package JSON/CSV 是否只包含一次 package 身份，而没有重复数据字段
- JSON governor/boost 是否在可用时为字符串/布尔值、不可用时为 `null`
- JSON / CSV 是否能被 helper scripts 正常读取
- 不可用传感器是否为 JSON `null` / CSV 空单元格，而真实零值仍为数值零
- JSON 是否从不输出 `NaN` 或无穷大 token

### Hotplug 恢复

在受控维护窗口运行多 interval 采集，通过平台的正常运维流程 offline、再
online 一个非 boot CPU；无论检查是否成功，都要恢复该 CPU。

重点看：

- CPU 成员变化是否被检测，即使总数量随后恢复到原值
- 重建边界是否只作为 baseline 消费，不会输出 0/100% 尖峰
- cpufreq、cpuidle、PMU 与 topology 状态是否跟随新的 tracked CPU 集
- 第一个可见 interval 前收到 `SIGINT` 或 `SIGTERM` 时，JSON 是否仍保持合法

### 画图 smoke check

```bash
python3 scripts/plot_sum.py summary.json --preset power-temp
python3 scripts/plot_cpu.py cpus.json --preset busy --top 8
```

重点看：

- helper scripts 是否能直接消费当前导出
- 画图链路是否和当前 `schema_version` 对齐

### 长时间稳定性

按计划用于生产的 interval 至少运行 30 分钟 summary 采集：

```bash
./armstat -S -a -f json -O soak.json -i 1 -n 1800
```

上面的 `ARMSTAT_SOAK_ITERATIONS=1800` target-test 已自动检查资源有界与导出
完整性。运行期间和结束后还要检查：

- 进程没有跳过 interval，也没有意外丢失部署所需的 telemetry
- 用 `SIGINT` 与 `SIGTERM` 中断另一次运行时，JSON 数组能闭合，PMU/sysfs
  descriptor 能释放

## 4. 已覆盖与未覆盖

目前已覆盖：

- in-tree/out-of-tree 构建、配置失效、安装、卸载与许可证安装
- 严格输入和内核文本解析
- 注入式读取失败、计数器复位、不可用值和恢复 baseline 路径
- 使用 synthetic CPU inventory 的 hotplug 依赖状态重建
- helper scripts 导入兼容性
- 机器可读导出契约稳定性

目前还没有自动化覆盖：

- 真实硬件上的 cpuidle 行为
- 特定 ARM 平台上的 PMU 可用性与 scaling 正确性
- hotplug 行为
- nohz_full 行为
- package 功耗 / 温度源发现

这些仍然需要目标机器实机验证。

## 5. 发布门槛

发布候选打 tag 前，从干净状态执行：

```bash
make clean
make test
make debug-test
make clean
make test
```

最后一次 `make test` 用来确认从 sanitizer flags 切回默认 fortified release
flags 时同样会正确重建。最终发布签字仍必须完成上面的目标服务器检查；仅通过
本地测试不能代替 ARM64 Linux 实机验证。
