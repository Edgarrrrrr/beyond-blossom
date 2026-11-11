# DECODER-DOCUMENT-MATRIX-20261106：新框架六策略全矩阵重测

## 1. 结果与版本

21 设置、六种最新策略及官方同轮对照全部完成；882 次百万调用逐条精确校验通过。
源码 `eb66e671947d066adafab1b2a504d45b2f0530d7` 的优化策略保持原样，LTO 全部关闭，最短实际预热 2.000000535 秒。
框架父版 DECODER-DOCUMENT-PROTOCOL-20261106，提交 `72fe618441aab1a7017ef5cdd4f32f062d4afe65`。
官方主分母为原样 v2.3.1 `e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd`；仓库 Upstream 为附加分母。

21 设置等权几何平均加速比（分母耗时 / 策略耗时）：

- Upstream：相对官方 0.999×；相对仓库 Upstream 1.000×。
- GridSIMD：相对官方 1.134×；相对仓库 Upstream 1.135×。
- Predecode：相对官方 4.226×；相对仓库 Upstream 4.232×。
- Component：相对官方 4.566×；相对仓库 Upstream 4.573×。
- TailReset：相对官方 4.601×；相对仓库 Upstream 4.608×。
- Spatial：相对官方 4.926×；相对仓库 Upstream 4.934×。

![官方主比较](official-speedup.png)

## 2. 测量对象

复用已验证的直接/具体类型入口及 LTO OFF 静态库。输入、DEM、前 1024 条循环预热、
计时边界和 Min3/Median3/Batch3 沿用文档协议。新增加的是完整矩阵外层参数与六节点调度。
每组七入口使用同节点和同输入，顺序轮换。本轮冻结实际节点映射：batch r1=ares1、r2=ares3、r3=ares5；latency r1=ares2、r2=ares4、r3=ares6。
逐轮记录见 rounds.csv。batch 和 latency 使用不同节点；两种模式的差异同时包含节点与模式因素，
不能单独归因于逐 shot 读钟。

## 3. 方法

d=9/13/17/21/25/29/33，p=0.05%/0.1%/0.3%，rounds=d，sigma=0。
21 份百万原始 v2 输入，root seed=2026101402，原始 DEM 哈希逐项一致。
每设置/入口 3 次完整百万 batch 和 3 次百万 latency。Batch3 包含末尾 drain；
主要延迟分布采用 latency r1/r2/r3 各轮实测。Min3 按原 shot 编号跨 ares2/4/6 三轮取最小值，
作为附加稳定化指标；逐 shot Median3 同时保留。跨节点聚合不代表任一节点的一轮实际延迟。
另保留管线最终 drain、各轮分布和 CPU 时间。
每个进程预热至少两秒，常驻团队创建退出、构图、加载、校验和文件输出在解码计时外。
全部作业仅 ares1–6，节点独占，测量 CPU0–7、控制 CPU8、NUMA0。

## 4. 计时与实现证据

解码核心和原生计时函数没有修改。[父框架计时函数交互页](/home/shzr/quantum/predecode_test/reproduction/PyMatching-experiments/.local/worktrees/DECODER-DOCUMENT-PROTOCOL-20261106/acquisition/experiments/decoder-document-protocol-20261106/timing.html)
及其直接入口/统一接口机器码证据保持原身份。全部六策略配置、二进制 SHA、实际构建命令
和采集冻结提交可由 plan.json 与 build 记录追溯。本轮没有新增优化或阶段插桩。

## 5. 完整结果

以下为 Batch3，单位 μs/shot；全部三轮保留。

| 设置 | Upstream | GridSIMD | Predecode | Component | TailReset | Spatial | Official |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| d09-p0.0005-s0.00 | 1.202 | 1.286 | 0.332 | 0.388 | 0.560 | 0.565 | 1.202 |
| d09-p0.0010-s0.00 | 2.592 | 2.538 | 0.677 | 0.758 | 0.920 | 0.963 | 2.613 |
| d09-p0.0030-s0.00 | 10.117 | 8.654 | 3.256 | 3.803 | 3.721 | 4.738 | 10.129 |
| d13-p0.0005-s0.00 | 3.931 | 3.828 | 0.807 | 1.029 | 1.167 | 1.327 | 3.994 |
| d13-p0.0010-s0.00 | 8.736 | 7.814 | 1.831 | 2.798 | 2.711 | 3.418 | 8.517 |
| d13-p0.0030-s0.00 | 33.868 | 27.274 | 10.482 | 8.886 | 8.759 | 9.634 | 33.897 |
| d17-p0.0005-s0.00 | 9.867 | 9.128 | 1.773 | 2.810 | 2.609 | 3.081 | 9.896 |
| d17-p0.0010-s0.00 | 21.003 | 18.947 | 4.357 | 4.860 | 4.644 | 4.880 | 20.886 |
| d17-p0.0030-s0.00 | 83.251 | 67.056 | 25.840 | 18.585 | 18.514 | 18.320 | 83.325 |
| d21-p0.0005-s0.00 | 19.788 | 18.419 | 3.548 | 4.389 | 4.035 | 3.887 | 19.756 |
| d21-p0.0010-s0.00 | 42.592 | 38.218 | 8.951 | 8.201 | 7.871 | 7.211 | 42.618 |
| d21-p0.0030-s0.00 | 175.356 | 139.387 | 53.109 | 35.062 | 34.826 | 32.328 | 175.905 |
| d25-p0.0005-s0.00 | 34.900 | 33.033 | 6.380 | 7.017 | 6.205 | 5.193 | 34.977 |
| d25-p0.0010-s0.00 | 76.430 | 69.636 | 16.518 | 13.747 | 12.898 | 10.452 | 75.184 |
| d25-p0.0030-s0.00 | 340.640 | 261.069 | 99.061 | 61.449 | 60.920 | 54.073 | 344.307 |
| d29-p0.0005-s0.00 | 55.611 | 54.060 | 10.769 | 10.403 | 9.440 | 6.872 | 55.501 |
| d29-p0.0010-s0.00 | 127.291 | 116.514 | 28.428 | 21.633 | 20.268 | 14.977 | 127.808 |
| d29-p0.0030-s0.00 | 611.937 | 438.488 | 173.680 | 100.147 | 98.902 | 85.019 | 602.815 |
| d33-p0.0005-s0.00 | 88.267 | 85.596 | 17.442 | 15.049 | 13.770 | 9.407 | 86.989 |
| d33-p0.0010-s0.00 | 219.540 | 185.594 | 46.568 | 32.371 | 30.399 | 21.405 | 219.409 |
| d33-p0.0030-s0.00 | 954.414 | 684.383 | 274.973 | 155.893 | 156.040 | 130.151 | 954.733 |

![仓库 Upstream 附加比较](repository-speedup.png)

[measurements.csv](measurements.csv) 同时列出 Min3、Median3、线程数和两个分母加速比；
[rounds.csv](rounds.csv) 保留各轮绝对值、实际节点、预热和 CPU 使用。
summary.json 保留各轮及聚合分布、分位数支持量、事件数分层、固定输入 0/356321/999999，
以及逐 shot 数组和所有分布图、CCDF 数据的 SHA256 引用。

每个实际 latency 轮次均将同节点的七策略绘在同一组直方图和 CCDF 中。
全部图共用 0.01..100000 μs 的 100 个几何 bin，summary 同时保留区间外计数。
CCDF 使用严格 P(L>t)，计算全部 distinct 实测阈值，保留完整尾部，不做阈值抽样或横轴截断。
对数纵轴显示全部正概率，经验分辨率为 1/1000000；最大阈值的零概率保存在
distributions/*-ccdf.npz 的阈值与 above 数组中，零值不在对数纵轴显示。
每条曲线概率由 above / count 复算，不向低于 1/1000000 的概率外推。
下表前三列为主要延迟分布，Min3 列保留附加指标；每个链接均包含七策略共享 bin 直方图与严格 CCDF。

| 设置 | 实测 latency r1 | 实测 latency r2 | 实测 latency r3 | 附加 Min3 |
| --- | --- | --- | --- | --- |
| d09-p0.0005-s0.00 | [r1 / ares2](distributions/d09-p0.0005-s0.00-latency-r1.png) | [r2 / ares4](distributions/d09-p0.0005-s0.00-latency-r2.png) | [r3 / ares6](distributions/d09-p0.0005-s0.00-latency-r3.png) | [Min3](distributions/d09-p0.0005-s0.00.png) |
| d09-p0.0010-s0.00 | [r1 / ares2](distributions/d09-p0.0010-s0.00-latency-r1.png) | [r2 / ares4](distributions/d09-p0.0010-s0.00-latency-r2.png) | [r3 / ares6](distributions/d09-p0.0010-s0.00-latency-r3.png) | [Min3](distributions/d09-p0.0010-s0.00.png) |
| d09-p0.0030-s0.00 | [r1 / ares2](distributions/d09-p0.0030-s0.00-latency-r1.png) | [r2 / ares4](distributions/d09-p0.0030-s0.00-latency-r2.png) | [r3 / ares6](distributions/d09-p0.0030-s0.00-latency-r3.png) | [Min3](distributions/d09-p0.0030-s0.00.png) |
| d13-p0.0005-s0.00 | [r1 / ares2](distributions/d13-p0.0005-s0.00-latency-r1.png) | [r2 / ares4](distributions/d13-p0.0005-s0.00-latency-r2.png) | [r3 / ares6](distributions/d13-p0.0005-s0.00-latency-r3.png) | [Min3](distributions/d13-p0.0005-s0.00.png) |
| d13-p0.0010-s0.00 | [r1 / ares2](distributions/d13-p0.0010-s0.00-latency-r1.png) | [r2 / ares4](distributions/d13-p0.0010-s0.00-latency-r2.png) | [r3 / ares6](distributions/d13-p0.0010-s0.00-latency-r3.png) | [Min3](distributions/d13-p0.0010-s0.00.png) |
| d13-p0.0030-s0.00 | [r1 / ares2](distributions/d13-p0.0030-s0.00-latency-r1.png) | [r2 / ares4](distributions/d13-p0.0030-s0.00-latency-r2.png) | [r3 / ares6](distributions/d13-p0.0030-s0.00-latency-r3.png) | [Min3](distributions/d13-p0.0030-s0.00.png) |
| d17-p0.0005-s0.00 | [r1 / ares2](distributions/d17-p0.0005-s0.00-latency-r1.png) | [r2 / ares4](distributions/d17-p0.0005-s0.00-latency-r2.png) | [r3 / ares6](distributions/d17-p0.0005-s0.00-latency-r3.png) | [Min3](distributions/d17-p0.0005-s0.00.png) |
| d17-p0.0010-s0.00 | [r1 / ares2](distributions/d17-p0.0010-s0.00-latency-r1.png) | [r2 / ares4](distributions/d17-p0.0010-s0.00-latency-r2.png) | [r3 / ares6](distributions/d17-p0.0010-s0.00-latency-r3.png) | [Min3](distributions/d17-p0.0010-s0.00.png) |
| d17-p0.0030-s0.00 | [r1 / ares2](distributions/d17-p0.0030-s0.00-latency-r1.png) | [r2 / ares4](distributions/d17-p0.0030-s0.00-latency-r2.png) | [r3 / ares6](distributions/d17-p0.0030-s0.00-latency-r3.png) | [Min3](distributions/d17-p0.0030-s0.00.png) |
| d21-p0.0005-s0.00 | [r1 / ares2](distributions/d21-p0.0005-s0.00-latency-r1.png) | [r2 / ares4](distributions/d21-p0.0005-s0.00-latency-r2.png) | [r3 / ares6](distributions/d21-p0.0005-s0.00-latency-r3.png) | [Min3](distributions/d21-p0.0005-s0.00.png) |
| d21-p0.0010-s0.00 | [r1 / ares2](distributions/d21-p0.0010-s0.00-latency-r1.png) | [r2 / ares4](distributions/d21-p0.0010-s0.00-latency-r2.png) | [r3 / ares6](distributions/d21-p0.0010-s0.00-latency-r3.png) | [Min3](distributions/d21-p0.0010-s0.00.png) |
| d21-p0.0030-s0.00 | [r1 / ares2](distributions/d21-p0.0030-s0.00-latency-r1.png) | [r2 / ares4](distributions/d21-p0.0030-s0.00-latency-r2.png) | [r3 / ares6](distributions/d21-p0.0030-s0.00-latency-r3.png) | [Min3](distributions/d21-p0.0030-s0.00.png) |
| d25-p0.0005-s0.00 | [r1 / ares2](distributions/d25-p0.0005-s0.00-latency-r1.png) | [r2 / ares4](distributions/d25-p0.0005-s0.00-latency-r2.png) | [r3 / ares6](distributions/d25-p0.0005-s0.00-latency-r3.png) | [Min3](distributions/d25-p0.0005-s0.00.png) |
| d25-p0.0010-s0.00 | [r1 / ares2](distributions/d25-p0.0010-s0.00-latency-r1.png) | [r2 / ares4](distributions/d25-p0.0010-s0.00-latency-r2.png) | [r3 / ares6](distributions/d25-p0.0010-s0.00-latency-r3.png) | [Min3](distributions/d25-p0.0010-s0.00.png) |
| d25-p0.0030-s0.00 | [r1 / ares2](distributions/d25-p0.0030-s0.00-latency-r1.png) | [r2 / ares4](distributions/d25-p0.0030-s0.00-latency-r2.png) | [r3 / ares6](distributions/d25-p0.0030-s0.00-latency-r3.png) | [Min3](distributions/d25-p0.0030-s0.00.png) |
| d29-p0.0005-s0.00 | [r1 / ares2](distributions/d29-p0.0005-s0.00-latency-r1.png) | [r2 / ares4](distributions/d29-p0.0005-s0.00-latency-r2.png) | [r3 / ares6](distributions/d29-p0.0005-s0.00-latency-r3.png) | [Min3](distributions/d29-p0.0005-s0.00.png) |
| d29-p0.0010-s0.00 | [r1 / ares2](distributions/d29-p0.0010-s0.00-latency-r1.png) | [r2 / ares4](distributions/d29-p0.0010-s0.00-latency-r2.png) | [r3 / ares6](distributions/d29-p0.0010-s0.00-latency-r3.png) | [Min3](distributions/d29-p0.0010-s0.00.png) |
| d29-p0.0030-s0.00 | [r1 / ares2](distributions/d29-p0.0030-s0.00-latency-r1.png) | [r2 / ares4](distributions/d29-p0.0030-s0.00-latency-r2.png) | [r3 / ares6](distributions/d29-p0.0030-s0.00-latency-r3.png) | [Min3](distributions/d29-p0.0030-s0.00.png) |
| d33-p0.0005-s0.00 | [r1 / ares2](distributions/d33-p0.0005-s0.00-latency-r1.png) | [r2 / ares4](distributions/d33-p0.0005-s0.00-latency-r2.png) | [r3 / ares6](distributions/d33-p0.0005-s0.00-latency-r3.png) | [Min3](distributions/d33-p0.0005-s0.00.png) |
| d33-p0.0010-s0.00 | [r1 / ares2](distributions/d33-p0.0010-s0.00-latency-r1.png) | [r2 / ares4](distributions/d33-p0.0010-s0.00-latency-r2.png) | [r3 / ares6](distributions/d33-p0.0010-s0.00-latency-r3.png) | [Min3](distributions/d33-p0.0010-s0.00.png) |
| d33-p0.0030-s0.00 | [r1 / ares2](distributions/d33-p0.0030-s0.00-latency-r1.png) | [r2 / ares4](distributions/d33-p0.0030-s0.00-latency-r2.png) | [r3 / ares6](distributions/d33-p0.0030-s0.00-latency-r3.png) | [Min3](distributions/d33-p0.0030-s0.00.png) |

## 6. 正确性与范围

882 次百万调用的原编号、事件数、整数权重和 observable 与原参考逐条精确一致。
正式前两端设置的 28 次 1025 条接口检查通过。输入记录共 2100 万，复用不会增加独立样本。
本轮是既有组合策略重测，没有同算法单线程扩展或阶段归因数据；总 CPU 包含忙等待，
不能全部解释为有效计算。保留所有轮次，不宣称性能无波动或统计显著。

## 7. 复现与证据

原始数据：/home/shzr/quantum/predecode_test/reproduction/PyMatching-experiments/.local/artifacts/DECODER-DOCUMENT-MATRIX-20261106/formal-r01/plan.json 所在目录。全部任务完成凭据、零退出文件、
原始 CSV、绑定/线程快照与文件 SHA 在 lanes/ 下可追溯。
脚本与复现说明见本实验 README；完整采集必须使用新 run，分析必须使用新输出目录。
本轮实际完成两端 smoke、全矩阵采集和原始数据分析，未新增飞书或网页发布。
