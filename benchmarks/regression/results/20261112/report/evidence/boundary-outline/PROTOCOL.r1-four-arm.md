# 单个边界处理函数的隔离验证

这是对三个已观察退化配置的后续机制实验。原始 819-call 结果和报告保持原样；本轮不增加官方上游比较，也不把新结果改写为原矩阵结果。

四个二进制为 `source-sealed`、`current-sealed`、`rebuilt-current`、`outlined-boundary`。前两者逐字节复用原迁移来源与 edd29b6 候选；后两者从 c5da46e 的同路径快照、同编译器/参数重新构建。最后一臂只给 `Mwpm::handle_tree_hitting_boundary` 添加 `[[gnu::noinline]]`。源码清单只能在 `src/beyond_blossom/structured/matcher/mwpm.inl` 有一处文件差异，实际补丁须仅含这个函数的属性变化。

三组固定输入直接复制原 run 的 dataset/model/circuit，不采样新输入：

| 输入与方法 | 执行模式 | CPU | 参数 |
| --- | --- | --- | --- |
| d17, p=.003, component-parallel | batch / shared | 2,4 | partitions=1, component_threshold=64, shot_threshold=32, conflict_width=3 |
| d17, p=.001, pipelined | pipe-batch / pipeline | 2,4,6,8 | partitions=1, component_threshold=64, shot_threshold=32, conflict_width=3 |
| d33, p=.0005, parallel-predecode | pipe-batch / pipeline | 2,4,6,8,10,12,14,16 | partitions=2, component_threshold=0, shot_threshold=32, conflict_width=0, parallel_import=1, component_group_owned, three_planes |

均为实际 AVX-512VL、10,000 条保存输入、1024-shot blocks、至少两秒预热。每次调用固定同一 resident team；普通 decode、结果写入和 pipeline 最后 drain 的范围与原采集器一致。内部统计关闭，正确性和CSV持久化位于计时外。保存输入的重复执行次数不增加独立输入记录数。

预先固定八轮，不删除异常值，不按收益提前结束。每轮轮转三种 case 的次序，每种 case 的四臂连续调用。设四臂为 A/B/C/D，顺序为：

1. A B C D
2. B C D A
3. C D A B
4. D A B C
5. D C B A
6. A D C B
7. B A D C
8. C B A D

每臂在每个位置恰好两次；每对臂的先后各四次。每轮 case 顺序左旋 `repeat % 3`，所以 case 位置次数是3/3/2；不声称八轮可做到三个位置完全均衡。总计三 case × 四臂 × 八轮 = 96 次 native 调用。任何退出码、整数权重、observable 见证、输入哈希、实际内核或线程放置验证失败立即终止并保留失败目录。失败 run 不生成完整汇总；须新建输出重新执行完整协议。

对照分别回答三个问题：

- `source-sealed / current-sealed`：原来源与当前候选的迁移差异。
- `current-sealed / rebuilt-current`：相同当前源码在新路径/重编译后的差异；这是变体归因的控制项。
- `rebuilt-current / outlined-boundary`：单个函数禁止内联的增量效果。

速度比均为前者耗时 / 后者耗时，大于1表示后者更快；同时报告两侧绝对 ns/shot、全部八次配对比值、观察范围以及耗时变化。主描述量为各臂八次 batch-mean 的中位数和中位数之比，配对比值中位数另列。观察范围不是置信区间；不根据固定阈值宣称性能等价或单项优化普遍成立。

## 准备与执行

构建完成后使用 `--prepare-only` 封存源身份、四个二进制、输入、协议、采集器、c5da AE helpers、完整96-call顺序及命令。此模式不调用 native，也不创建 raw 测量。输出必须是新目录。

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
