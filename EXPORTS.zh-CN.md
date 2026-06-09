# armstat 导出契约说明

<p align="center">
  <a href="README.zh-CN.md">← 返回 README</a> |
  <a href="DESIGN.zh-CN.md">设计</a> |
  <a href="PLOTTING.zh-CN.md">画图</a> |
  <a href="TESTING.zh-CN.md">测试</a>
</p>

本文档说明 `armstat` 产生的机器可读导出格式。

适用场景：

- 编写下游解析脚本
- 对接 pandas、Excel、画图脚本
- 判断导出格式修改是否兼容

当前导出契约版本：

- `schema_version = 4`

## 范围

`armstat` 目前支持两种机器可读导出：

- JSON：`armstat -f json`
- CSV：`armstat -f csv`

两种格式都以“区间”为单位。每个导出记录都对应一个已经完成的采样区间。

## 稳定元数据

每个导出区间都带有以下元数据：

- `schema_version`
- `interval`
- `timestamp`
- `timestamp_iso`

含义：

- `schema_version`
  - 导出契约版本号
- `interval`
  - 从 1 开始的样本序号
- `timestamp`
  - Unix 时间戳（秒）
- `timestamp_iso`
  - 同一采样时刻的 ISO 8601 字符串

`timestamp` 更适合程序处理。  
`timestamp_iso` 更适合人工阅读、日志比对和手工排查。

## JSON 格式

JSON 输出的顶层是一个数组。数组中的每个元素都是一个区间对象。

示例：

```json
[
  {
    "schema_version": 4,
    "interval": 1,
    "timestamp": 1774665600,
    "timestamp_iso": "2026-03-28T10:40:00+0800",
    "cpus": [
      {
        "cpu": 0,
        "freq": 2200.00,
        "idle_percent": 99.90,
        "busy_percent": 0.10
      }
    ],
    "summary": {
      "avg_freq": 2200.00,
      "uncore_freq": 1600.00,
      "temp0": 45.00,
      "temp1": 44.00,
      "idle_percent": 99.80,
      "busy_percent": 0.20
    }
  }
]
```

在 `schema_version = 4` 中，summary 温度键统一成 `temp0`、`temp1`……
不再使用 `temp_vdie0`、`temp_vdie1`……；split idle 键统一成 `lpi0`、
`lpi1`……，不再使用 `sum_idle_state0`、`sum_idle_state1`……。

同样地，CPU 级导出也统一成更简洁的键名，例如：

- `freq`、`min`、`max`
- `node`
- `temp`
- `idle_percent`、`iowait_percent`、`busy_percent`
- `lpi0`、`lpi1`……

### JSON 对象结构

每个区间对象都一定包含：

- `schema_version`
- `interval`
- `timestamp`
- `timestamp_iso`

它还可能包含：

- `cpus`
- `packages`
- `summary`

`cpus`、`packages` 和 `summary` 是否存在，取决于当前输出模式：

- 默认 CPU 导向 JSON
  - 通常包含 `cpus`
  - 如果显式启用了 summary 级字段，也可能同时包含 `summary`
- `-S` 摘要 JSON
  - 包含 `summary`
  - 不包含 `cpus`

### JSON CPU 对象

每个 CPU 对象一定包含：

- `cpu`

还可能包含：

- 当前启用的 CPU 级字段
- 如果启用了 PMU，则包含 `pmu`

字段是否出现受当前列选择影响，不能假设所有 CPU 字段总是同时存在。

### JSON package 对象

当 package 级字段启用时（例如 `-s pkg`），每个 interval 可能包含
`packages` 数组：

```json
"packages": [
  { "package": 0, "freq": 2200.00, "idle_percent": 95.0, "busy_percent": 5.0 }
]
```

每个 package 对象一定包含 `package`（socket 编号）。

### JSON summary 对象

`summary` 对象包含当前启用的 summary 级字段。  
如果启用了 PMU，并且存在聚合后的摘要 PMU 值，还可能包含：

- `pmu`

### 空值

JSON 对不可用值使用 `null`，例如：

- 不可用的温度
- 不可用的 PMU 值
- 被 disable 或不支持的 split idle state

## CSV 格式

CSV 是按行输出的。表头只在开始输出一次。

每一行的前四列固定是：

- `schema_version`
- `interval`
- `timestamp`
- `timestamp_iso`

之后的列布局会变成以下三种之一：

- 仅 summary 行布局
- 仅 CPU 行布局
- summary 和 CPU 同时启用时的 mixed-scope 行布局

### Summary CSV

典型命令：

```bash
armstat -S -f csv -O summary.csv
```

表头形状：

```text
schema_version,interval,timestamp,timestamp_iso,SUM,...
```

数据行特点：

- 每个 interval 一行
- 行身份列为 `SUM`
- 后面跟当前启用的 summary 级字段
- 如果启用了 PMU，最后还会追加聚合后的 PMU 列

### CPU CSV

典型命令：

```bash
armstat -f csv -O cpus.csv
```

表头形状：

```text
schema_version,interval,timestamp,timestamp_iso,CPU,...
```

数据行特点：

- 每个 interval 中，每个导出的 CPU 一行
- `CPU` 列保存真实 CPU ID
- 后面跟当前启用的 CPU 级字段
- 如果启用了 PMU，最后还会追加 PMU 列

### Mixed-scope CSV

典型命令：

```bash
armstat -f csv -s cpu,power -O mixed.csv
```

表头形状：

```text
schema_version,interval,timestamp,timestamp_iso,Scope,CPU,...
```

数据行特点：

- 每个 interval 会输出一行 summary，再输出零到多行 CPU
- `Scope=SUM` 的行承载 summary 级字段
- `Scope=CPU` 的行承载 CPU 级字段
- 当 `Scope=SUM` 时，`CPU` 为空
- mixed-scope CSV 的表头会加前缀，避免 summary/CPU 同名列冲突：
  - `summary.<field>`：summary 字段
  - `cpu.<field>`：CPU 字段
  - `summary.pmu.<event>` / `cpu.pmu.<event>`：PMU 字段
- 不属于该行 scope 的字段会输出为空单元格

这样 mixed-scope CSV 的语义就和 text/JSON 保持一致，不再静默丢掉某一侧字段。

### CSV 引号规则

CSV 会在必要时自动加引号。典型情况包括：

- 包含逗号
- 包含双引号
- 包含换行
- 包含回车

因此下游工具应该按正常 CSV 解析，不要自己简单按逗号切分。

## 字符串与转义

JSON 字符串在输出前会做转义。  
CSV 字段在必要时会做引用。

这意味着下游脚本可以安全处理：

- governor 名称
- PMU 事件名
- ISO 时间戳

而不需要依赖“当前这些字符串刚好很简单”。

## Summary 与 CPU 导出

适合用 summary 导出的场景：

- 每个 interval 只要一条记录
- 关注全系统趋势
- 想画 `SUM` 级时间序列

适合用 CPU 导出的场景：

- 想看 per-CPU 时间序列
- 需要 CPU 排序、过滤、分组
- 想画 CPU 级曲线

## 兼容性建议

`schema_version` 应该被视为兼容性门槛。

同一个 schema 版本内，通常允许做“增量式兼容修改”，例如：

- 增加新的可选字段
- 增加新的 PMU 事件名
- 在硬件支持时增加新的温度线
- 增加像 `uncore_freq` 这样的可选平台级 summary 字段

如果修改涉及：

- 删除字段
- 重命名字段
- 根本性改变结构

就应该提升 schema 版本号。

## 安装位置

如果通过 `make install` 安装 `armstat`，本文档会被安装到：

```text
share/doc/armstat/EXPORTS.zh-CN.md
```

相关文档：

- `PLOTTING.zh-CN.md`
- `TESTING.zh-CN.md`
- `README.zh-CN.md`
