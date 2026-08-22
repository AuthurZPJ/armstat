# armstat 画图说明

<p align="center">
  <a href="README.zh-CN.md">← 返回 README</a> |
  <a href="EXPORTS.zh-CN.md">导出格式</a>
</p>

本文档专门说明 `armstat` 附带的画图脚本。

这部分内容和 `armstat` 主命令刻意分离：

- `armstat` 负责采集和导出区间数据
- 附带脚本负责把导出的数据画成时间序列图

当前附带脚本有：

- `scripts/plot_sum.py`：`SUM` 级画图
- `scripts/plot_cpu.py`：CPU 级画图

两个脚本都从 `scripts/armstat_loader.py` 导入共享的加载器和字段别名，
从 `scripts/plot_utils.py` 导入 matplotlib 辅助工具。两个脚本都支持读取
`armstat` 的 JSON 导出，也支持当前版本的 CSV 导出。

更精确的机器可读导出结构，请参考独立文档 `EXPORTS.zh-CN.md`。

如果使用 `make install` 安装 `armstat`，这些 helper scripts 和画图文档
也会一起安装到 `share/doc/armstat/` 下。

JSON 输入会把整个导出文件读入内存。CSV 输入配合 `--sample-range` 时只加载请求的样本。因此更适合：

- 中等规模 trace
- 交互式分析
- 经过过滤后的 CPU 导出

如果 trace 很长、或者 CPU 导出非常大，建议优先：

- 先画 `SUM` 级图
- 在 CPU 图里使用 `--cpu-filter`、`--top` 或 `--group-by`
- 尽量缩小导出时间窗口

## 依赖

这些脚本需要 matplotlib。安装方式：

```bash
python3 -m pip install matplotlib
```

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
- `duration_us`
- `timestamp`
- `timestamp_ns`
- `timestamp_iso`

所以画图时可以直接使用真实时间轴。

这些时间字段表示同一个采样时刻，只是格式不同：

- `timestamp`：Unix 整秒，为兼容旧消费者保留
- `timestamp_ns`：Unix 纳秒，画图与亚秒输入优先使用
- `timestamp_iso`：带小数秒的 RFC 3339 时间字符串

`duration_us` 是实测采样窗口。loader 会把它保留为下游分析元数据，但不会
默认把它当作可画指标。

画图脚本优先使用 `timestamp_ns`，读取 schema 4/5 时回退到 `timestamp`。
保留 `timestamp_iso` 的目的，是让 CSV/JSON 导出在人工检查、日志比对和
外部系统对接时更直观。

当前机器可读导出的版本号是 `schema_version = 7`。附带 loader 同时接受
version 4 至 7，因此已有 trace 仍可继续使用。读取 mixed CSV 的 SUM / CPU
图时，loader 会跳过 `PKG` 行与 `package.*` 列。
JSON/CSV 的精确结构请参考 `EXPORTS.zh-CN.md`。

schema 5 引入的不可用值（JSON `null`、CSV 空单元格）会被加载成 `NaN`，图中因此
显示断点而不是虚假的零值；平滑与分组聚合在仍有有效数据点时会忽略缺失点。
package 功耗或内存带宽因来源发现有歧义而禁用时也遵循该规则。

如果 armstat 在 stderr 报告 CPU inventory 截断，CPU 图只包含实际导出的可
表示 ID（`0..1023`）。loader 不会为更高 ID 合成曲线；在把该图视为全机覆盖
前，应减少源机器 CPU 集或使用更大的 `MAX_CPUS` 重新构建。

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
- `--y freq` 在 summary 导出中解析为 `avg_freq`，在 CPU 导出中解析为
  `freq`
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
- `-o, --output PATH`

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

- `freq` -> CPU 数据的 `freq`（summary 数据中为 `avg_freq`）
- `temp` -> `temp`
- `busy` -> `busy_percent`
- `idle` -> `idle_percent`
- `iowait` -> `iowait_percent`
- `lpi1` -> `lpi1`
- `cycles` -> `pmu.cycles`
- `instructions` -> `pmu.instructions`

armstat 会在采样前拒绝重复 PMU 事件名，因此 PMU 事件路径没有歧义。

### CPU 选择

- `--cpu-filter 0,1,4-7`
  - 显式指定 CPU 列表，语法类似 `armstat -c`
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
- `-o, --output PATH`

### 说明

- CPU 图当前聚焦 `armstat` 已经按 CPU 导出的字段
- 目前还没有 per-CPU 功耗导出
- 在大机器上，分组图通常比逐 CPU 图更容易看

### CPU CSV 示例

```bash
python3 tools/power/armstat/scripts/plot_cpu.py cpus.csv --cpu-filter 0,2,4 --y freq
python3 tools/power/armstat/scripts/plot_cpu.py cpus.csv --preset busy --top 8
```
