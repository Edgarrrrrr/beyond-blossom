# 单个边界处理函数的隔离验证（r2：固定官方参考 + 四控制臂）

这是对三个已观察退化配置的后续机制实验。原始 819-call 结果和报告保持原样；本轮保留固定官方 e27 的正常单线程参考，并将新比较作为单独的120-call实验，不改写原矩阵。r1四臂协议保存在 PROTOCOL.r1-four-arm.md；它未执行。

五个二进制为 `source-sealed`、`current-sealed`、`rebuilt-current`、`outlined-boundary`、`official-sealed`。官方臂逐字节复用原819-call run中e27参考，使用generic/serial、CPU2。前两者逐字节复用原迁移来源与 edd29b6 候选；两个重编译控制从 c5da46e 的同路径快照、同编译器/参数重新构建。outlined-boundary 只给 `Mwpm::handle_tree_hitting_boundary` 添加 `[[gnu::noinline]]`。源码清单只能在 `src/beyond_blossom/structured/matcher/mwpm.inl` 有一处文件差异，实际补丁须仅含这个函数的属性变化。

三组固定输入直接复制原 run 的 dataset/model/circuit，不采样新输入：

| 输入与方法 | 执行模式 | CPU | 参数 |
| --- | --- | --- | --- |
| d17, p=.003, component-parallel | batch / shared | 2,4 | partitions=1, component_threshold=64, shot_threshold=32, conflict_width=3 |
| d17, p=.001, pipelined | pipe-batch / pipeline | 2,4,6,8 | partitions=1, component_threshold=64, shot_threshold=32, conflict_width=3 |
| d33, p=.0005, parallel-predecode | pipe-batch / pipeline | 2,4,6,8,10,12,14,16 | partitions=2, component_threshold=0, shot_threshold=32, conflict_width=0, parallel_import=1, component_group_owned, three_planes |

四个结构化控制臂使用实际 AVX-512VL；所有五臂均用10,000 条保存输入、1024-shot blocks、至少两秒预热。每次调用固定同一 resident team；普通 decode、结果写入和 pipeline 最后 drain 的范围与原采集器一致。内部统计关闭，正确性和CSV持久化位于计时外。保存输入的重复执行次数不增加独立输入记录数。

预先固定八轮，不删除异常值，不按收益提前结束。每轮轮转三种 case 的次序，每种 case 的五臂连续调用。设上述五臂为 A/B/C/D/E，顺序为：

1. A B C D E
2. B C D E A
3. C D E A B
4. D E A B C
5. E A B C D
6. E D C B A
7. A E D C B
8. B A E D C

前五轮完整循环轮转，后三轮反转前三轮。每臂在每个位置出现1或2次，并非完全均衡；每对臂先后次数为3/5或4/4。严格四对完全镜像会让中心位置计数均为偶数，不能同时满足五臂每个位置至少一次，因此本协议明确保留这项有限平衡。第 r 轮 case 顺序左旋 `(r-1) % 3`，r从1开始，case位置次数是3/3/2。总计三 case × 五臂 × 八轮 = 120 次 native 调用。任何退出码、整数权重、observable 见证、输入哈希、实际内核或线程放置验证失败立即终止并保留失败目录。失败 run 不生成完整汇总；须新建输出重新执行完整协议。

官方臂在每个case使用该case同一份dataset/model/circuit及10,000条输入，固定generic/serial和CPU2；不会套用相邻并行方法的线程/分区参数。其预热、1024-shot blocks、正常decode清理和校验口径沿用原官方契约。

对照分别回答三个问题：

- `source-sealed / current-sealed`：原来源与当前候选的迁移差异。
- `current-sealed / rebuilt-current`：相同当前源码重编译后的控制。本次两者benchmark、core库和Stim库哈希已证实完全相同，因此此比较检验重复执行与顺序变化，不再声称路径代码生成差异。
- `rebuilt-current / outlined-boundary`：单个函数禁止内联的增量效果。

另保留 `official-sealed / 各控制臂` 的同输入主参考比较。单函数归因仍只使用 rebuilt-current / outlined-boundary。

速度比均为前者耗时 / 后者耗时，大于1表示后者更快；同时报告两侧绝对 ns/shot、全部八次配对比值、观察范围以及耗时变化。主描述量为各臂八次 batch-mean 的中位数和中位数之比，配对比值中位数另列。观察范围不是置信区间；不根据固定阈值宣称性能等价或单项优化普遍成立。

## 准备与执行

构建完成后使用 `--prepare-only` 封存源身份、五个二进制、输入、协议、采集器、c5da AE helpers、完整120-call顺序及命令。此模式不调用 native，也不创建 raw 测量。输出必须是新目录。

```sh
WORK=/tmp/beyond-boundary-outline-20261112
python3 "$WORK/run_confirmatory.py" --prepare-only --output "$WORK/prepared-run"
```

只有收到执行授权、所有构建结束后，才运行：

```sh
python3 "$WORK/run_confirmatory.py" --execute --output "$WORK/prepared-run"
```

已准备目录只能执行一次。`--execute` 也可指定全新输出，在同一进程先准备后执行；此显式选项是唯一会调用 native 的入口。`--summarize-only` 只接受已完成且所有封存文件未变化的 run。`--self-test` 使用临时微型文件及 mock invoke 检查顺序和失败约束，不运行任何解码器。

`plan.json`、`prepared.json`、`started.json`、`complete.json` 或 `failure.json` 明确区分准备、开始、完整结束和失败。所有执行只读取本轮已复制的文件；原 819-call run、主仓库和原二进制均不修改。
