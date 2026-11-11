# 当前 TailReset：预解码三策略重测

论文使用： [数字宏生成及接入说明](predecoder_macros.md)。
`generate_predecoder_macros.py` 从本轮已完成证据生成全部 T、N、R 及范围边界，正文与表格共用相同键。

`tailreset_collect.py` 从当前仓库源码构建 TailReset，支持冻结、Slurm 构建及端点检查、完整采集、汇总。
`tailreset_data.py` 将完成的时间和 syndrome 结果打包进 AE，并可离线核验。
本入口替代历史采集器对 2026-10-24 旧二进制的固定引用。

## 三种策略与条件编译

| 策略 ID | 中文名 | `PYMATCHING_TAILRESET_PREDECODE_MODE` |
| --- | --- | --- |
| `skip` | 不预解码 | `1` |
| `coordinate` | 按坐标顺序预解码 | `2` |
| `degree` | 当前按度数顺序预解码 | `0`（默认） |

三种配置共用当前 TailReset 的双缓冲、P1、两个 X/Z 组件和正常 MWPM；每个缓冲组两个物理核，X/Z 各一个，两个缓冲组共四个物理核。
`skip` 保留输入规范化、共享 seed 和正常状态导入，所有规范化 seed 以零半径、未匹配状态进入 MWPM。
`coordinate` 只排序源索引，按嵌入网格的线性 cell 次序 `(t, gy, gx)` 处理；旋转网格使用实际嵌入坐标。
坐标索引准备、必要排序和预解码都计入时间。其邻域函数只生成候选 mask。
`degree` 保留当前融合的叶节点处理、稳定度数链和构造期绑定的 AVX2 内核。
本实验比较完整策略，时间差包含贪心顺序、剩余状态和后续 MWPM 的变化。

纯计时库和调用者均使用 `PYMATCHING_TAILRESET_SYNDROME_STATS=OFF`（默认）。
采样结构、逐 seed 扫描、双缓冲快照和读取 API 全部由 `#if PM_TAILRESET_SYNDROME_STATS` 包裹。
统计版单独构建为 `ON`，只接受 `pipe-statistics`，拒绝生成纯计时结果。
统计发生在实际双缓冲 `prepare_for_mwpm` 完成后、`solve_prepared` 前，包含小输入回退路径。
统计版继续完成解码并校验全部结果；它的耗时不参与性能表。

默认度数策略的两份核心 translation unit 与适配前采用源码比较预处理 token，另检查纯库中没有统计符号。
这项检查证明新增统计在关闭时消失；实际检查结果保存在本次 run 的 `build/degree/disabled-instrumentation.json`。

## 冻结的测量口径

- `d=9,13,17,21,25,29,33`，`p=0.003`，`rounds=d`，相对噪声标准差为零。
- 每设置复用同一份 100 万独立输入；seed、stream、原始 DEM 和参考结果保留 SHA256。
- 每策略每设置 3 轮完整 batch（每块 1024 条）和 1 轮逐 shot 延迟；统计版独立重放 1 轮。
- 固定官方 PyMatching v2.3.1 `e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd` 同轮执行 3 batch + 1 latency，作为附加绝对时间与加速比分母。
- 全部 LTO OFF；每进程预热至少 2 秒；常驻 OpenMP 团队包围预热和连续测量，启动/退出在计时外。
- 完整 batch 包含派发、等待、结果存储和 reset，最终 drain 包含在最后一块中。逐 shot 延迟另外保存最终 drain，完整总时间包括它。
- 模型构造、输入加载、结果校验、统计重放和 CSV 写入在性能计时外。
- 同一个测量单元在同一节点轮转四种入口；不同重复可在不同节点执行。结果报告逐轮值，不把重复当作新增独立输入。
- 限定 ares1–6，每节点一个独占作业。原始 per-shot CSV、block CSV、进程 CPU/RSS、绑核和 NUMA 回执保留在 run 中。

Batch 均值为 `sum(三轮 elapsed_ns) / 3,000,000 / 1000`，单位 `μs/shot`。
剩余数为预解码后未匹配的规范化 seed，X/Z 相加；同时保存百万 shots 总数和每 shot 均值。
每条统计记录必须满足 `normalized = residual + 2*pairs + boundaries`，并逐组件核验。
不预解码的剩余数必须等于规范化初始数。

每条输入核验原 shot ID、事件数和整数最优权重。等权 observable 差异保存逐条记录，
在相同 P1 组件算法上导出实际匹配，并与被测输出比较，再在原始图上独立核验奇偶性、权重和 observable。
缺少一致纠正路径、权重差异、输入变化或覆盖缺失都会停止该作业，失败证据保留。

## 服务器完整重测

依赖：完整 Git 仓库、C++20/OpenMP 编译器、CMake、原固定 Stim 源码和 archive、固定 Googletest 源码，
以及原 `DECODER-DOCUMENT-MATRIX-20261106/formal-r01` 的输入/参考数据。
这些大型材料和服务器构建工具通过参数传入；只复制 AE 可离线核验小型结果，无法重新采集。
`--implementation` 必须是当前 `src` 对应的完整 commit。

```bash
python3 AE/tailreset_collect.py freeze \
  --output /absolute/new-run \
  --cache-dir /absolute/new-build-cache \
  --implementation FULL_SOURCE_COMMIT \
  --reference-run /absolute/DECODER-DOCUMENT-MATRIX-20261106/formal-r01 \
  --stim-source /absolute/stim \
  --gtest-source /absolute/googletest \
  --tool-runtime /absolute/pinned-tools \
  --nodes ares2,ares3,ares4,ares5,ares6
python3 /absolute/new-run/source/AE/tailreset_collect.py submit --stage build --output /absolute/new-run
```

`--tool-runtime` 在系统工具齐备的环境可省略；服务器指定带 `manifest.json` 的既有固定 CMake/CTest/Git runtime。
工具库只传给构建子进程，测量时清理 `LD_LIBRARY_PATH`、`LD_PRELOAD` 以及外部 PM/OMP 配置。
构建阶段执行 6 组、每组 48 个穷举 syndrome/顺序/复用检查、默认核心回归，以及大/小距离各 1025 条 batch、latency 和 statistics 检查。
需要纯 smoke 时只运行上面的 build 阶段，检查三份 `build/*/ready.json` 和对应退出文件均为零。

完整阶段：

```bash
python3 /absolute/new-run/source/AE/tailreset_collect.py submit --stage formal --output /absolute/new-run
python3 /absolute/new-run/source/AE/tailreset_collect.py submit --stage analysis --output /absolute/new-run
```

提交器为正式作业设置全部构建的成功依赖，为分析设置全部正式作业的成功依赖。
构建节点取传入列表的前三个，避免固定依赖繁忙的 ares1。
查询仅使用 `squeue` / `scontrol show job`；完成状态以日志、`exit-JOBID.txt`、逐调用验证和完成凭据核验。
每次新采集必须使用新 run/cache，旧 run 不覆盖。

## 输出、离线核验与重绘

完成 run 的 `analysis-r01/summary.csv` 输出三种策略和官方对照的逐轮时间、三轮均值、syndrome 总数/均值和直接完成 shots 数。
`latency.csv` 保存独立逐 shot 时间分位数及最终 drain；`summary.json` 保留全部 133 次调用的来源。
`predecode.csv` 自动转换成现有 AE 绘图的十行 schema，历史 ID `current` 对应本轮 `degree`。

```bash
python3 AE/tailreset_data.py bundle --run /absolute/completed-run --output /absolute/new-bundle
python3 AE/tailreset_data.py verify --source /absolute/new-bundle/SOURCE.json
python3 AE/tailreset_data.py verify
python AE/plot.py predecode --output /absolute/new-figures
python3 -m unittest discover -s AE -p 'test_*.py'
```

包内保留来源 plan、全部完成/退出凭据、验证/预热/正确性记录、原生时间聚合及 syndrome 整数计数。
大输入、原始百万行 CSV 和二进制按原路径及 SHA256 留在服务器工件目录；离线验证不会重新解码，也不会重新读取未打包的原始 CSV。
数据更新后同步 `data/SHA256SUMS`、`data/SOURCES.md` 及本轮验证记录，历史数据另存，其他 AE 数据保持其冻结身份。
