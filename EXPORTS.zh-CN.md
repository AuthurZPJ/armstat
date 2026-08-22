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

- `schema_version = 7`

与下游兼容相关的版本历史：

- version 4 引入了简洁的温度与 split-idle 键名
- version 5 明确表示不可用数值，不再把它们序列化成看似可信的零值
- version 6 为 CSV 增加 package-only 与 mixed package 行，以及明确的
  `Scope`、`CPU`、`Package` 身份列，并为亚秒采集增加纳秒样本时间
- version 7 增加实测 interval 时长，使用 RFC 3339 时区偏移，为 summary
  CSV 提供正常的 `Scope` 身份列，去除重复 package 身份字段，并明确字符串
  与布尔值的机器可读缺失语义

## 范围

`armstat` 目前支持两种机器可读导出：

- JSON：`armstat -f json`
- CSV：`armstat -f csv`

两种格式都以“区间”为单位。每个导出记录都对应一个已经完成的采样区间。

## 稳定元数据

每个导出区间都带有以下元数据：

- `schema_version`
- `interval`
- `duration_us`
- `timestamp`
- `timestamp_ns`
- `timestamp_iso`

含义：

- `schema_version`
  - 导出契约版本号
- `interval`
  - 从 1 开始的样本序号
- `duration_us`
  - monotonic clock 实测的采样窗口微秒数；做归一化或审计时应使用它，而不是
    命令行请求的 interval
- `timestamp`
  - Unix 时间戳（秒）
- `timestamp_ns`
  - Unix 时间戳（纳秒）；亚秒对齐应优先使用此字段
- `timestamp_iso`
  - 同一采样时刻的 RFC 3339 字符串；schema 7 带 9 位小数，UTC 偏移中含冒号

`timestamp_ns` 是首选程序时间轴；`timestamp` 为需要整秒的旧消费者保留。
`timestamp_iso` 更适合人工阅读、日志比对和手工排查。

## JSON 格式

JSON 输出的顶层是一个数组。数组中的每个元素都是一个区间对象。

示例：

```json
[
  {
    "schema_version": 7,
    "interval": 1,
    "duration_us": 1000123,
    "timestamp": 1774665600,
    "timestamp_ns": 1774665600123456789,
    "timestamp_iso": "2026-03-28T10:40:00.123456789+08:00",
    "cpus": [
      {
        "cpu": 0,
        "freq": 2200.00,
        "governor": "schedutil",
        "boost": true,
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

从 `schema_version = 4` 起，summary 温度键统一成 `temp0`、`temp1`……
不再使用 `temp_vdie0`、`temp_vdie1`……；split idle 键统一成 `lpi0`、
`lpi1`……，不再使用 `sum_idle_state0`、`sum_idle_state1`……。后续版本
继续沿用这些键名。

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
- `duration_us`
- `timestamp`
- `timestamp_ns`
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
  - 不包含 `packages`

### JSON CPU 对象

每个 CPU 对象一定包含：

- `cpu`

还可能包含：

- 当前启用的 CPU 级字段
- 如果启用了 PMU，则包含 `pmu`

字段是否出现受当前列选择影响，不能假设所有 CPU 字段总是同时存在。

CPU 数组只包含 tracked 且可表示的 CPU 集。当前构建可表示 Linux CPU ID
`0..1023`；发现被截断时，armstat 会向 stderr 写警告，不会为不可表示的 ID
虚构导出行。

### JSON package 对象

当 package 级字段启用时（例如 `-s pkg`），每个 interval 可能包含
`packages` 数组：

```json
"packages": [
  { "package": 0, "freq": 2200.00, "idle_percent": 95.0, "busy_percent": 5.0 }
]
```

每个 package 对象一定包含 `package`（topology 报告的物理 package/socket
编号）。可选字段 ID `pkg_id` 控制这一身份字段，不会再把它作为数据字段重复
序列化一次。

### JSON summary 对象

`summary` 对象包含当前启用的 summary 级字段。  
启用 PMU 输出时还可能包含：

- `pmu`

未配置事件、PMU 未激活或任一 tracked CPU 没有完整 interval 时，值为 `null`。
PMU 事件名是对象键；CLI 校验会拒绝重复名称，因此这些键保持唯一。

### 缺失值与布尔值

从 schema version 5 起，不可用数值使用 `null`，例如：

- 当前频率读取失败后的频率字段
- procstat 数据不可用时的 Busy/Idle、iowait 与系统计数
- 当前来源不可用时的功耗、能量、内存带宽和稀疏温度传感器
- package 功耗或内存带宽发现到多个歧义候选来源时（armstat 不会非确定性选择
  第一个匹配项）
- PMU 未激活时的 IPC（CPU 和 summary）
- 不可用或正在重新建立 baseline 的 split idle state（`lpi0`-`lpi7`）
- state 缺失、被禁用或不可使用时的 split-idle 唤醒字段
- PMU 未激活或任一 tracked CPU 没有完整 interval 时的 `pmu` 对象

来源有效时的真实零值仍输出为数值 `0`/`0.00`。下游画图或聚合时不得把
`null` 强制转换成零。serializer 也会把内部所有非有限浮点值统一转成
`null`，因此 JSON 不会产生非标准的 `NaN` 或无穷大 token。

schema version 7 也会把 `governor` 等不可用字符串写成 `null`，而不是空字符串。
内核能提供 boost 状态时，`boost` 为 `true` 或 `false`；不可用时为 `null`。
text 和 CSV 对可用 boost 状态仍使用惯例中的 `1` / `0`。

## CSV 格式

CSV 是按行输出的。表头只在开始输出一次。

每一行的前六列固定是：

- `schema_version`
- `interval`
- `duration_us`
- `timestamp`
- `timestamp_ns`
- `timestamp_iso`

之后的列布局会变成以下四种之一：

- 仅 summary 行布局
- 仅 package 行布局
- 仅 CPU 行布局
- summary、package、CPU 中至少两个 scope 同时启用时的 mixed-scope 布局

### Summary CSV

典型命令：

```bash
armstat -S -f csv -O summary.csv
```

表头形状：

```text
schema_version,interval,duration_us,timestamp,timestamp_ns,timestamp_iso,Scope,...
```

数据行特点：

- 每个 interval 一行
- `Scope` 行身份列为 `SUM`
- 后面跟当前启用的 summary 级字段
- 如果启用了 PMU，最后还会追加聚合后的 PMU 列

### CPU CSV

典型命令：

```bash
armstat -f csv -O cpus.csv
```

表头形状：

```text
schema_version,interval,duration_us,timestamp,timestamp_ns,timestamp_iso,CPU,...
```

数据行特点：

- 每个 interval 中，每个导出的 CPU 一行
- `CPU` 列保存真实 CPU ID
- 后面跟当前启用的 CPU 级字段
- 如果启用了 PMU，最后还会追加 PMU 列

### Package CSV

典型命令：

```bash
armstat -f csv -s pkg_avg_freq -O packages.csv
```

表头形状：

```text
schema_version,interval,duration_us,timestamp,timestamp_ns,timestamp_iso,Package,...
```

数据行特点：

- 每个 interval 中，每个发现的 package 一行
- `Package` 列保存物理 package/socket ID
- 后面跟当前启用的 package 级字段
- package 精确字段 ID 会自动开启该 section，因此选择 `pkg_avg_freq`
  不会静默得到只有表头的空文件
- PMU 没有 package scope，因此不会追加到这些行

### Mixed-scope CSV

典型命令：

```bash
armstat -a -f csv -O mixed.csv
```

表头形状：

```text
schema_version,interval,duration_us,timestamp,timestamp_ns,timestamp_iso,Scope,CPU,Package,...
```

数据行特点：

- 每个 interval 按 `SUM`、`PKG`、`CPU` 顺序输出已启用的 scope
- `Scope=SUM` 的行承载 summary 级字段
- `Scope=PKG` 的行承载 package 级字段
- `Scope=CPU` 的行承载 CPU 级字段
- `CPU` 只在 `Scope=CPU` 时有值
- `Package` 只在 `Scope=PKG` 时有值
- mixed-scope CSV 的表头会加前缀，避免不同 scope 的同名列冲突：
  - `summary.<field>`：summary 字段
  - `package.<field>`：package 字段
  - `cpu.<field>`：CPU 字段
  - `summary.pmu.<event>` / `cpu.pmu.<event>`：PMU 字段
- 不属于该行 scope 的字段会输出为空单元格
- 不可用的数值或字符串字段同样输出为空单元格；有效零值仍是数值零；所有
  内部非有限浮点值也按不可用处理

这样 mixed-scope CSV 的语义就和 text/JSON 保持一致，不再静默丢掉某个
scope。schema 4/5 的 mixed CSV 只有 `Scope,CPU`，也没有 `PKG` 行；schema 6
引入当前 mixed 身份布局。下游应使用 `schema_version` 区分这些布局。

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

## 类型、单位与精度

`armstat --list` 是所有精确字段 ID 的权威发现视图。它从 serializer 使用的
同一字段 descriptor table 输出 scope、数据类型、单位、text 标签和 JSON 键。

规范单位如下：

- 频率：`MHz`
- 利用率与 idle residency：`%`
- 温度：`degC`
- package 功耗：`mW`
- interval 能量：`J`
- 内存带宽：`MiB/s`（每秒字节数除以 1024²）
- idle-state 唤醒：`/s`
- 上下文切换与中断：`count/interval`
- IPC：`instructions/cycle`

拓扑身份、governor 名称和 CPU/package ID 没有物理单位。`Boost` 类型为布尔值。
每 interval 计数不带小数尾数；其他数值的显示精度由字段定义，不能把它当作
传感器准确度声明。

## 选择导出 scope

适合用 summary 导出的场景：

- 每个 interval 只要一条记录
- 关注全系统趋势
- 想画 `SUM` 级时间序列

适合用 CPU 导出的场景：

- 想看 per-CPU 时间序列
- 需要 CPU 排序、过滤、分组
- 想画 CPU 级曲线

适合用 package 导出的场景：

- 关注 socket 级频率或利用率
- 希望每个 package 只占一行，避免更大的 per-CPU 数据集
- 对比不同 socket 的拓扑级差异

## 兼容性建议

`schema_version` 应该被视为兼容性门槛。

已支持 version 4 的消费者通常只需确保 JSON `null` 和 CSV 空单元格按缺失
数据处理，而不是按零处理，即可接受 version 5。
version 6 JSON 增加 `timestamp_ns`，其 section 结构仍沿用 version 5；CSV
消费者还需接受 `timestamp_ns` 元数据列，以及 mixed 输出中可选的 `Package`
身份列、`package.*` 字段和 `PKG` 行。version 7 增加 `duration_us`；把
`timestamp_iso` 偏移从 `+hhmm` 改为 RFC 3339 `+hh:mm`；把 summary-only
CSV 身份表头从 `SUM` 改为 `Scope`；package 身份只序列化为
`package`/`Package`；`boost` 使用 JSON 布尔值；不可用字符串使用 JSON
`null`。随附的 summary/CPU 画图 loader 接受 version 4 至 7，并忽略目标
scope 之外的行。

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
