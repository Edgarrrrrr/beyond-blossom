# 论文宏与最新矩阵复现

## 当前预解码三策略表（2026-11-11）

[生成与接入说明](predecoder_macros.md) · [最新数字宏](predecoder_numbers.tex)。
独立脚本 `generate_predecoder_macros.py` 核验当前 TailReset 完成包，生成 56 项表格指标和 2 项正文范围边界。
原脚本仍计算下面记录的 28 项矩阵/PMU 宏；本表使用新的 `xeon.predecoder-comparison.*` 命名空间。
时间从三轮整数 ns 重算，N 从整数剩余数重算，R 从原始初始/剩余总数计算；末步统一舍入到两位。

```bash
python3 AE/generate_predecoder_macros.py --output AE/predecoder_numbers.tex
python3 AE/generate_predecoder_macros.py --check
```

导言区按顺序 `\input{paper_num_template.tex}`、`\input{predecoder_numbers.tex}`，论文正文和表格由外部论文项目维护。
原有 33 项数值及 M4 区段保持原样；旧模板中关于表格为固定数字文本的注释已更新。

## 原矩阵与 PMU 宏

`migrate_paper_numbers.py` 使用 Python 3.11+ 标准库。默认数据已随 AE 打包，单独复制 AE 到其他目录后可离线使用；绘图另需 `requirements.txt`。

```bash
python3 AE/migrate_paper_numbers.py --check
python3 AE/migrate_paper_numbers.py --output /absolute/path/paper_numbers.tex
python3 AE/migrate_paper_numbers.py --template /absolute/path/paper_num_template.tex --in-place
python AE/plot.py latest-matrix --output /absolute/path/new-figures
python3 -m unittest discover -s AE -p test_migrate_paper_numbers.py -q
python3 AE/pmu_data.py verify
python3 -m unittest discover -s AE -p 'test_*.py' -q
```

`--check` 无写入；生成文件旁附逐项 `.audit.json`。`--in-place` 先保存 `.bak-<原SHA前16位>`，再原子写回。
保持所有非数字文本、注释、CRLF 和每条宏的小数位；Decimal 60 位中间精度，最后一步 ROUND_HALF_UP。
所有首段含 m4 的键及整个 M4 区段原样保留，禁止给 M4 配置更新规则。
不认识的新字段、未确认映射、不完整数据、SHA 不匹配会退出 2，拒绝写回或退回旧数据。

## 已确认的字段含义

| 字段/论文名 | 对应实测数据 |
| --- | --- |
| pymatching | 仓库 Upstream，T1，LTO OFF |
| structured-simd | GridSIMD，T1 |
| predecoding | Predecode，T1 |
| component-parallel | Component，T2 |
| tail-reset | TailReset，T4 |
| parallel-predecoding | Spatial，T8 |
| best | 五个候选逐设置最快，按每个 p 对七个 d 取最大加速比 |
| TailReset 改善设置数 | 未舍入 Batch3 严格优于 Component 的设置数 |
| `xeon.ablation.d33.p0p3.*.L3miss` | 本轮独立L3 PMU run：原始miss数/读取请求数×100 |
| `xeon.ablation.d33.p0p3.*.L2miss` | 本轮独立L2 PMU run：原始miss数/读取请求数×100 |
| 单核摘要 | Predecode 在当前 21 设置的等权几何平均和最大加速比 |
| 八核摘要 | Spatial 在当前 21 设置的最大加速比 |
| d33/p0.1% block/round | Spatial 八核每 shot 的完整 33 轮 QEC 时间，以及该值除以 33 |

摘要保留 `legacy` 命名空间兼容论文调用，原 d23 两项已改为 `legacy.d33.p0p1.beyond-blossom.*`，旧 d23 名字不再定义。
此处 QEC block 是一个完整 shot，区别于计时器每块 1024 shots 的 batch block。

`xeon.decoder-comparison.*` 也使用本轮六策略内部矩阵，不读取历史跨算法的 `data/decoders.csv`。
`plot.py decoders` 仍保留那张历史图；替换外部算法数据需要另行确认其来源。

## 数据与统计

本包来自 `DECODER-DOCUMENT-MATRIX-20261106/formal-r01/analysis-r02`，实现
`eb66e671947d066adafab1b2a504d45b2f0530d7`，官方对照
`e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd`，全部 LTO OFF。
d=9/13/17/21/25/29/33，p=0.05%/0.1%/0.3%，rounds=d，sigma=0。
每种入口每设置 3×百万 batch + 3×百万 latency，共 882 次百万调用；输入仅 2100 万条，重复执行不增加独立样本。

上述统计用于既有24个时间/加速宏。新增4个PMU宏使用[独立L2/L3数据包](data/pmu/SOURCE.json)，
每缓存级别21设置×Upstream/GridSIMD×1轮百万shots，共84次调用；没有延迟读钟。
`pmu_miss_percent`规则逐项指定缓存级别、percent单位和d/p，按整数计数计算，再保留三位小数。
默认值分别为L3 `0.094/0.017`、L2 `60.212/26.988`。L2分母按Intel定义包含L1D硬件预取器请求。
可用`--pmu-data /absolute/new-pmu-bundle/SOURCE.json`显式选择新完成包；仍核验全部矩阵、来源、事件及SHA。
PMU重新采集及导出命令见[PMU说明](pmu.md)。

宏及两张最新热力图均使用 `Batch3 = 三轮 batch 整数总纳秒 / 3000000 / 1000`，单位 μs/shot。
末尾管线 drain 已计入最后 batch block；每次预热至少 2 秒。
宏分母固定仓库 Upstream，另一张图明确使用 Official。Min3/Median3 与逐轮分布留在原完整分析，不用于这些宏。

`data/latest-matrix/originals/` 保留原完整分析 summary、完成凭据、CSV 和六节点完成记录的原字节；
`run/plan.json` 同样保持原字节。`run/analysis-batch/` 和 `run/lanes/` 为明确标记的相对路径派生视图。
`SOURCE.json` 逐一记录原路径、原 SHA 和包内路径。绘图输入、126 格参考表与 21 个最快项均由同一份 summary 生成。
重新导出包不会改写原始 run，且拒绝覆盖已有输出。

只依赖本包能验证已有凭据、重算宏和图；不包含原始逐 shot/block CSV、输入、二进制与分布 NPZ。
这一级复现不重复解码或重新验证百万条正确性。外部工件的绝对路径只用于溯源，不是默认宏/画图依赖。

## 从原始采集导出新分析或数据包

以下命令需服务器上的完整原始 run。替换为真实路径；输出目录须不存在。

```bash
python3 AE/collect_paper_batch_source.py --run /absolute/formal-run --output /absolute/new-batch-analysis
python3 AE/migrate_paper_numbers.py --run /absolute/formal-run --analysis /absolute/new-batch-analysis --check
python3 AE/bundle_paper_source.py --run /absolute/formal-run --analysis analysis-r02 --output /absolute/new-offline-bundle
```

收集器重新核验 882 份 validation 及原生小文件，并逐块求和 441 份 batch CSV；大逐 shot 文件沿用正式采集完成的校验哈希链。
打包器需要完整分析的 summary/complete、分析零退出、六节点完成与零退出，核验三轮整数耗时和 147 个 Batch3 单元。
更换实验身份、实现或矩阵范围时，先明确修改映射；不要仅改路径来混合旧数据。
