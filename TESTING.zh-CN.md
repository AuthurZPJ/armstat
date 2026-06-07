# armstat 测试说明

本文档说明当前 `armstat` 的测试方式。

## 目标

当前测试策略分成两层：

- 任意开发机上都能快速执行的本地回归检查
- 需要在目标 ARM 服务器上执行的实机验证

第一层主要保护代码结构和机器可读导出契约。  
第二层主要验证 cpuidle、PMU、NUMA 温度映射、package 功耗等平台相关行为。

## 1. 本地构建检查

在 `tools/power/armstat` 目录下执行：

```bash
make clean
make
```

这一步主要确认：

- C 源码仍然可以正常构建
- formatter / export 路径仍然可以正确链接
- helper scripts 与当前源码树仍然兼容

如果要做内存或未定义行为排查，也可以用 sanitizer 版本：

```bash
make debug
```

这个目标会用 AddressSanitizer 和 UBSan 重新构建 `armstat`，更适合本地
调试，不作为常规 release 构建方式。

## 2. Smoke Test

执行：

```bash
make test
```

当前会运行：

- `tests/test_core_logic`
- `tests/test_column_selection`
- `tests/test_runtime_smoke`
- `tests/test_cli_smoke.sh`
- `tests/test_plot_loaders.py`

这些 smoke test 保持得很小、很快，主要验证：

- idle 百分比、busy 百分比、iowait 和 schedstat clamp 计算正确

- `-s` / `-H` 的精确字段选择不会误伤整组列
- `-a` / `all` 只会打开基础列组，不会隐式打开 PMU / IPC
- `-H all` 也会把显式启用过的 PMU / IPC 一起关闭
- `-S -a`、`-a -I`、`--probe`、busy-source 解析这些主流程组合仍然正常
- JSON / CSV serializer 的 summary-only / mixed-scope 输出仍然能端到端生成
- summary JSON 仍能被 `scripts/plot_sum.py` 读取
- summary CSV 仍能被 `scripts/plot_sum.py` 读取
- CPU JSON 仍能被 `scripts/plot_cpu.py` 读取
- CPU CSV 仍能被 `scripts/plot_cpu.py` 读取
- 当前机器可读导出仍符合 `schema_version = 4`

这些测试 **不会** 验证 ARM 平台上的运行时语义。

## 3. 目标 ARM 服务器实机验证

下面这些命令是最值得在真实机器上做的手工检查。

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

### Idle / Busy 语义

```bash
./armstat -i 1 -n 3 -s cpu,LPI-0,LPI-1,Idle%,IOWait%,Busy%
./armstat -i 1 -n 3 -S --busy-source procstat
./armstat -i 1 -n 3 -S --busy-source schedstat
```

重点看：

- `Idle% + Busy%` 是否接近 `100`
- `IOWait%` 是否独立显示
- 可见 `LPI-*` 之和是否接近 `Idle%`
- 在 `nohz_full` CPU 上，`schedstat` 是否比 `procstat` 更稳定

### PMU / IPC

```bash
./armstat -i 1 -n 2 -I
./armstat -i 1 -n 2 -S -p cycles,instructions
```

重点看：

- 请求 PMU 时列是否正常出现
- PMU 不可用时是否清晰降级，而不是静默消失
- `-I` 是否不会破坏 Busy/Idle

### NUMA / 温度 / 功耗

```bash
./armstat -i 1 -n 2 -s cpu,pkg,core,node,temp
./armstat -i 1 -n 2 -S -s power,temp,energy
./armstat --probe
```

重点看：

- CPU 行是否映射到正确 NUMA 节点
- CPU 温度是否符合预期 `Temp0..` summary 温度映射
- package 功耗是否只出现在 `SUM` 级
- `--probe` 是否反映真实平台传感器模型，并显示
  `summary_temp_policy`

### 导出验证

```bash
./armstat -S -f json -O summary.json -n 2
./armstat -S -f csv -O summary.csv -n 2
./armstat -f json -O cpus.json -n 2
./armstat -f csv -O cpus.csv -n 2
```

重点看：

- 导出里是否包含 `schema_version`
- CSV 是否包含 `timestamp` 和 `timestamp_iso`
- JSON / CSV 是否能被 helper scripts 正常读取

### 画图 smoke check

```bash
python3 scripts/plot_sum.py summary.json --preset power-temp
python3 scripts/plot_cpu.py cpus.json --preset busy --top 8
```

重点看：

- helper scripts 是否能直接消费当前导出
- 画图链路是否和当前 `schema_version` 对齐

## 4. 已覆盖与未覆盖

目前已覆盖：

- 构建正确性
- helper scripts 导入兼容性
- 机器可读导出契约稳定性

目前还没有自动化覆盖：

- 真实硬件上的 cpuidle 行为
- 特定 ARM 平台上的 PMU 可用性与 scaling 正确性
- hotplug 行为
- nohz_full 行为
- package 功耗 / 温度源发现

这些仍然需要目标机器实机验证。
