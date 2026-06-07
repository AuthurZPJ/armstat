# armstat 画图说明

本文档专门说明 `armstat` 附带的画图脚本。

这部分内容和 `armstat` 主命令刻意分离：

- `armstat` 负责采集和导出区间数据
- 附带脚本负责把导出的数据画成时间序列图

当前附带脚本有：

- `scripts/plot_sum.py`：`SUM` 级画图
- `scripts/plot_cpu.py`：CPU 级画图

两个脚本都支持读取 `armstat` 的 JSON 导出，也支持当前版本的 CSV 导出。

更精确的机器可读导出结构，请参考独立文档 `EXPORTS.zh-CN.md`。

如果使用 `make install` 安装 `armstat`，这些 helper scripts 和画图文档
也会一起安装到 `share/doc/armstat/` 下。

这些 helper scripts 当前会把整个导出文件读入内存，因此更适合：

- 中等规模 trace
- 交互式分析
- 经过过滤后的 CPU 导出

如果 trace 很长、或者 CPU 导出非常大，建议优先：

- 先画 `SUM` 级图
- 在 CPU 图里使用 `--cpu-filter`、`--top` 或 `--group-by`
- 尽量缩小导出时间窗口

## 输入导出

### Summary 导出

```bash
armstat -S -f json -O summary.json
armstat -S -f csv -O summary.csv
```

### CPU 导出

```bash
armstat -f json -O cpus.json
armstat -f csv -O cpus.csv
```

当前 CSV 已经自带：

- `schema_version`
- `interval`
- `timestamp`
- `timestamp_iso`

所以画图时可以直接使用真实时间轴。

这两个时间字段表示的是同一个采样时刻，只是格式不同：

- `timestamp`：Unix 时间戳（数字，适合程序处理）
- `timestamp_iso`：ISO 8601 时间字符串（更适合人工阅读）

画图脚本主要使用 `timestamp`。保留 `timestamp_iso` 的目的，是让
CSV/JSON 导出在人工检查、日志比对和外部系统对接时更直观。

当前机器可读导出的版本号是 `schema_version = 4`。
JSON/CSV 的精确结构请参考 `EXPORTS.zh-CN.md`。

## Summary 画图

用 `plot_sum.py` 画 `SUM` 级时间序列。

### 常见命令

```bash
python3 tools/power/armstat/scripts/plot_sum.py summary.json --preset freq
python3 tools/power/armstat/scripts/plot_sum.py summary.json --preset power
python3 tools/power/armstat/scripts/plot_sum.py summary.json --preset temp
python3 tools/power/armstat/scripts/plot_sum.py summary.json --preset power-temp
python3 tools/power/armstat/scripts/plot_sum.py summary.json --preset idle-lpi
python3 tools/power/armstat/scripts/plot_sum.py summary.json --preset sysstat

python3 tools/power/armstat/scripts/plot_sum.py summary.json --y busy
python3 tools/power/armstat/scripts/plot_sum.py summary.json --y power --y2 temp0
python3 tools/power/armstat/scripts/plot_sum.py summary.json --list-fields
```

### Summary 预设

- `freq`
- `power`
- `temp`
- `power-temp`
- `idle-lpi`
- `sysstat`

说明：

- `--preset freq` 会画 `avg_freq`，如果导出里带有 `uncore_freq`，也会一起
 画出来
- `--preset temp` 会把所有可用的 `temp*` 一起画出来
- `--preset power-temp` 会在左轴画 `power`，右轴画所有可用的
  `temp*`
- `--preset idle-lpi` 适合看 `Busy% / Idle% / LPI-*` 的变化
- `--preset sysstat` 适合看 `CtxSw / IRQs / SoftIRQs / MemBW`

### 常用选项

- `--y FIELD`
- `--y2 FIELD`
- `--sample-range START:END`
- `--smooth N`
- `--output-dir DIR`
- `--format png|svg|pdf`
- `--title TEXT`

例如：

```bash
python3 tools/power/armstat/scripts/plot_sum.py summary.json --preset idle-lpi --sample-range 10:200 --smooth 5
python3 tools/power/armstat/scripts/plot_sum.py summary.json --preset sysstat --output-dir plots --format svg
python3 tools/power/armstat/scripts/plot_sum.py summary.csv --y freq
```

## CPU 画图

用 `plot_cpu.py` 画 CPU 级时间序列，或者画按组聚合后的 CPU 图。

### 常见命令

```bash
python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --preset freq --cpu-filter 0,2,4
python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --preset temp --cpu-filter 0,2,4
python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --preset busy --top 8
python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --preset busy --top 8 --rank-by max

python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --y busy --cpu-filter 0,2,4
python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --y temp --cpu-filter 0-31 --top 4
python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --y freq --y2 temp --cpu-filter 0
python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --y lpi1 --cpu-filter 0,2,4

python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --group-by node --y busy
python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --group-by core --y temp --top 8
python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --group-by node --aggregate max --y temp

python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --y busy --sample-range 10:200 --smooth 5 --cpu-filter 0-31 --top 8
python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --preset freq --output-dir plots --format svg --top 4

python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --list-fields
python3 tools/power/armstat/scripts/plot_cpu.py cpus.json --list-cpus
```

### CPU 预设

- `freq`
- `temp`
- `idle`
- `busy`
- `iowait`
- `ipc`

### 字段别名

脚本除了接受导出里的原始字段名，也支持更顺手的别名。

例如：

- `freq` -> `freq`
- `temp` -> `temp`
- `busy` -> `busy_percent`
- `idle` -> `idle_percent`
- `iowait` -> `iowait_percent`
- `lpi1` -> `lpi1`
- `cycles` -> `pmu.cycles`
- `instructions` -> `pmu.instructions`

### CPU 选择

- `--cpu-filter 0,1,4-7`
  - 显式指定 CPU 列表，语法和 `armstat -c` 一致
- `--top N`
  - 按主轴字段选前 N 个实体
- `--rank-by avg|max|last`
  - 控制 `--top` 的排序方式：
    - `avg`：时间平均值
    - `max`：峰值
    - `last`：最后一个可见样本

对于大核机器，如果直接把所有 CPU 都画出来，通常会不可读。因此实践上建议使用：

- `--cpu-filter`
- `--top`
- `--group-by`

### 分组画图

`--group-by` 可以把多个 CPU 聚合成一条线。

当前支持：

- `--group-by node`
- `--group-by core`

组内聚合方式支持：

- `--aggregate avg`
- `--aggregate max`
- `--aggregate min`

语义是：

- 先按组聚合
- 如果同时用了 `--top`，排序对象就变成 group，而不是单个 CPU
- 每个样本点都会先把组内 CPU 的目标字段按指定方式聚合，再画出该组这一条线

### 其他常用选项

- `--y FIELD`
- `--y2 FIELD`
- `--sample-range START:END`
- `--smooth N`
- `--output-dir DIR`
- `--format png|svg|pdf`
- `--title TEXT`

### 说明

- CPU 图当前聚焦 `armstat` 已经按 CPU 导出的字段
- 目前还没有 per-CPU 功耗导出
- 在大机器上，分组图通常比逐 CPU 图更容易看

### CPU CSV 示例

```bash
python3 tools/power/armstat/scripts/plot_cpu.py cpus.csv --cpu-filter 0,2,4 --y freq
python3 tools/power/armstat/scripts/plot_cpu.py cpus.csv --preset busy --top 8
```
