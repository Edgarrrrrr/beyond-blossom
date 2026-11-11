# 离线阅读与复现

直接打开 `index.html`。四个 `mechanisms/*.html`、`code-diff/index.html` 和 `boundary-diff/index.html` 均内嵌脚本与样式，不需要网络、CDN或本地服务器。三个采用机制与一个未采纳变体分别提供独立页面。`summary.json` 是图、正文表和机制效果区唯一的数值来源。原始源码与中文解释分别保存在 `sources/`；原迁移patch覆盖清单声明的函数与片段，后续变体patch完整覆盖唯一变化的文件。

本报告的来源角色为：

| 角色 | 固定身份 |
| --- | --- |
| 迁移来源 | 原仓库 `241160314087e5ae698c6e76c3c055a11c3ad422` |
| 本地恢复提交 | `source-2411603` → `9651df809d8c2c034717bd3b036c35fda1c90fcb` |
| 被测运行时 | `edd29b6780713dc78f6a99d67c4c4d192e9c52b5` |
| 加入官方计时的采集器 | `0ad4fde55adaae41e337a6ee89974703c07bb8cd`，仅采集器三文件变化 |
| 独立官方上游 | PyMatching `e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd` |
| Stim | `1320ad7eac7de34d2e9c70daa44fbc6d84174450` |

工作区归档提供完整 run 目录和大文件。本紧凑报告包含小型原始CSV、源码节选与回执；回执中保留采集时的绝对路径和哈希。这些路径是来源记录，不是承诺在另一台机器上存在的下载地址。

## 仅重算

把归档解压后设置三个实际 run 路径，不改写原始CSV。

```sh
REPORT=/absolute/path/beyond-final-report
FAST=/absolute/path/generic-final-fast
DENSE=/absolute/path/generic-final-dense
STRUCTURED=/absolute/path/structured-final-run
python3 "$REPORT/scripts/summarize_results.py" \
  --generic-fast "$FAST" --generic-dense "$DENSE" --structured "$STRUCTURED"
python3 "$REPORT/scripts/build_pages.py"
python3 "$REPORT/scripts/build_followup.py"
python3 "$REPORT/scripts/prepare_overview.py"
python3 "$REPORT/scripts/render_summary.py" "$REPORT/overview.json" \
  --output "$REPORT/figures/overview" --require-local-sources --pdf \
  --font /path/to/NotoSansCJK-Regular.ttc
python3 "$REPORT/scripts/build_report.py"
node "$REPORT/scripts/test_models.js"
python3 "$REPORT/scripts/verify_artifacts.py"
```

汇总器验证完成状态、raw哈希、完整矩阵键、整数计时、输入身份和小型构建凭据，并重算批次中位数和比值。它保留大输入与二进制的采集哈希，不把轻量分析描述为重新执行native正确性或重哈希全部大文件。绘图需要 matplotlib 和中文字体；本机字体为 `/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc`。布局与 diff 生成器随报告封存，来源是本轮使用的 performance-experiment 工具，其源码哈希见最终 manifest。

只查看或重算数值不需要浏览器。修改交互页后，用 `python3 "$REPORT/scripts/check_browser_files.py" --prepare` 生成直接 Chromium 命令，执行生成的 `browser-verification/run.sh`，再运行同一脚本的 `--collect`。此方式不需要调试端口，实际检查18个场景及1440/760像素布局；临时测试页追加DOM事件检查，截图打开未修改的交付页面。输出包括截图和 `browser-verification.json`，页面改变后旧回执失效。`seal_report.py` 在数值、静态和浏览器回执一致时生成完整文件清单。

## 未采纳的边界函数变体

后续120次调用是独立测量窗口，使用原矩阵中三个设置的30,000条保存记录。每个设置五臂、八次进程重复，重复执行不会新增独立记录。`evidence/boundary-outline/` 包含原始CSV、完成收据、冻结协议、完整控制器、各臂身份、完整原/变体头文件和实际ELF静态分析。变体身份为c5da快照加单行 `[[gnu::noinline]]` 补丁，没有对应已合入运行时提交。

`build_followup.py` 默认从随包小数据重算120行，核验完成哈希、执行顺序、输入身份、八次配对数值和唯一源码变化，然后更新同一 `summary.json` 的 `boundary_outline` 字段。原 `generic` 和 `structured` 字段保持不变。该操作不重新验证大二进制或执行解码器。

```sh
python3 "$REPORT/scripts/build_followup.py"
python3 "$REPORT/scripts/build_report.py"
node "$REPORT/scripts/test_models.js"
```

完整运行目录与源快照由工作区的boundary-outline归档保存，位置、SHA256、2495成员校验及异路径重算结果见[持久归档清单](evidence/validation/boundary-outline-archive.json)。归档中的冻结控制器支持 `--prepare-only` 和显式 `--execute`，并拒绝再次执行已开始的目录。新采集需要重建或恢复各臂及其收据、原主机CPU放置和同输入，再创建新输出；本报告没有把它扩展为完整21设置的新矩阵。原819-call与后续120-call结果不合并，观察范围也不作为统计等价证明。

固定代码节选已包含在 `sources/`。如需从 Git 再导出，运行 `extract_sources.py --repo /path/to/repo`、`annotate_sources.py`，然后把已有 `code-diff/` 移到另一处，再运行 `build_diff.py`。源文件是精确字节副本，中文说明保存在独立清单。代码导出不会重新构建或执行解码器。

## 最小验证

在固定版本仓库根目录按 `AE/README.md` 准备 Python 环境、固定官方参考和 candidate 构建记录。安装包是外部DEM转换器的前提；从源码包构建的最终wheel已在仓库外验证。

```sh
WORK=/absolute/path/ae-work
python AE/reproduce.py run --profile smoke --backend avx512vl \
  --binary "$WORK/native/bin/bb_benchmark" --build-record "$WORK/native/build.json" \
  --reference-binary "$WORK/reference/build/bin/experiment_benchmark" \
  --reference-record "$WORK/reference/reference.json" \
  --shots 1025 --distances 9 --error-rates .003 --output "$WORK/new-native-smoke"
python AE/external/collect.py --ae-run "$WORK/new-native-smoke" \
  --build "$WORK/external-build" --profile smoke --limit 64 --repeats 1 \
  --cpus 2,4,6,8 --output "$WORK/new-external-smoke"
python AE/export_results.py --profile smoke --native-run "$WORK/new-native-smoke" \
  --external-run "$WORK/new-external-smoke" --groups comparison ablation \
  --output "$WORK/new-smoke-export"
```

示例核号来自本机socket0；换机器后选择同一socket的不同物理核。已有目录会拒绝覆盖。成功产物包含plan、correctness、原始测量、完整receipt和显式smoke标记的图。本轮这条闭环已执行；缺Python包和跨socket核列表的两次失败被保留，并在安装最终wheel与修正核号后重试成功。

## 完整迁移复核

在干净副本中构建固定来源和候选库；实际编译器、flags、依赖与链接命令以run的build receipt为准。本轮为GCC12.2、C++20、Release `-O3`、LTO/profile关闭。`benchmarks/regression/README.md` 给出 `build_benchmarks.py` 与 `build_structured.py` 的固定构建入口；官方参考必须单独用 `AE/build_reference.py` 构建，不能用当前generic二进制代替。

对于原有语料，用归档输入建立可移植manifest，重新采集到全新目录：

```sh
python3 "$REPORT/scripts/replay_generic_inputs.py" --run "$FAST" --output "$WORK/fast-inputs"
python3 "$REPORT/scripts/replay_generic_inputs.py" --run "$DENSE" --output "$WORK/dense-inputs"
python3 benchmarks/regression/run_generic.py \
  --baseline-bin "$WORK/regression-bin/baseline" --candidate-bin "$WORK/regression-bin/candidate" \
  --corpora "$WORK/fast-inputs" --candidate-identity 'commit:edd29b6780713dc78f6a99d67c4c4d192e9c52b5' \
  --cpu 2 --repeats 7 --batches-per-process 20 --output "$WORK/new-generic-fast"
python3 benchmarks/regression/run_generic.py \
  --baseline-bin "$WORK/regression-bin/baseline" --candidate-bin "$WORK/regression-bin/candidate" \
  --corpora "$WORK/dense-inputs" --candidate-identity 'commit:edd29b6780713dc78f6a99d67c4c4d192e9c52b5' \
  --cpu 2 --repeats 7 --batches-per-process 1 --output "$WORK/new-generic-dense"
python3 benchmarks/regression/run_structured.py \
  --build "$WORK/structured-regression-bin" \
  --reference-binary "$WORK/reference/build/bin/experiment_benchmark" \
  --reference-record "$WORK/reference/reference.json" \
  --cpus 0,2,4,6,8,10,12,14 --shots 10000 --repeats 3 \
  --output "$WORK/new-structured-regression"
```

语料生成器 `generate_corpora.py` 与 `generate_holdouts.py` 可生成新的矩阵；生成器随机种子和输出字节需要记录。重放保存语料直接验证本轮输入身份。保存记录数和同一语料的20次批处理、7次进程启动分别计数。

结构化默认21设置、六个来源/候选方法及一个官方参考，共819次调用；预热下限本身约27.3分钟，总耗时更长。正式计时前结束构建与其他计时任务。官方速度比 = 官方单线程正常解码耗时 / 候选耗时，并行候选分别使用2/4/8线程。Source迁移速度比 = Source耗时 / 候选耗时，按同名同线程配置配对。

本轮没有采集逐shot计时分位数，也没有重新采集M4或论文百万-shot全矩阵。相关AE入口仍可运行，结果必须使用新的run身份。
