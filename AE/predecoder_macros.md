# 预解码表格与正文的数字宏

[生成脚本](generate_predecoder_macros.py) · [已生成的 58 项数字宏](predecoder_numbers.tex) ·
[来源映射](predecoder_macro_source.json) · [验证记录](PREDECODER-MACROS-VALIDATION.json)。

使用最新 `AE-TAILRESET-20261110/formal-r04` 完整结果：当前 TailReset、T4、LTO OFF、
p=0.3%、七个距离，每项百万独立 shots。时间由三轮 batch 的整数总纳秒重算；剩余数来自独立统计重放的整数计数。

```bash
python3 AE/generate_predecoder_macros.py --output AE/predecoder_numbers.tex
python3 AE/generate_predecoder_macros.py --check
```

脚本仅需 Python 3.11+ 标准库。默认来源为随 AE 打包的 `data/tailreset/SOURCE.json`，
先核验固定 SHA、实验/源码身份、完整 133 次调用和包内 2008 份证据，再生成数值。
不需要原始 run、Slurm、解码器二进制或原始百万行 CSV。
`--output` 同时生成逐宏 `.audit.json`，保留原始整数、公式、验证凭据和未舍入值。
`--check` 无写入，检查已提交宏文件是否为最新结果；AE 可独立运行，无须论文文件。
另加 `--manuscript /path/to/manuscript.tex` 可核验外部正文/表格所用的全部 58 个键。
来源不完整、SHA 不符、策略映射不符或重复/缺失调用都会退出 2，拒绝写入。
重复生成可以刷新本脚本的生成文件；其他已有文件、数据来源及旧宏文件均受覆盖保护。

## 论文接入

AE 提供数据、生成脚本和数字宏，论文正文及表格由外部论文项目维护。
将两个数字宏文件导入或复制到论文项目，在导言区按以下顺序加载：

```latex
\input{paper_num_template.tex}
\input{predecoder_numbers.tex}
```

正文和表格通过 `\PerfValue{键名}` 引用数值，按下方规则选择指标键。
`paper_num_template.tex` 提供现有 `\DefinePerfValue` / `\PerfValue` 接口；
`predecoder_numbers.tex` 只增加新命名空间，并支持重复加载。

新宏不会修改既有 33 项论文数字和 M4 区段。原矩阵宏继续使用其冻结来源，
当前预解码表单独使用最新三策略数据，避免混合不同 run 的数值。

## 键与统计口径

键格式为 `xeon.predecoder-comparison.d33.p0p3.degree-ordered.mean-time-us`。

| 论文列 | 宏策略名 | 原生统计方法 |
| --- | --- | --- |
| None | `none` | `skip`，当前 TailReset 不预解码 |
| Simple | `coordinate` | `coordinate`，按嵌入坐标顺序 |
| Degree-ordered | `degree-ordered` | `degree`，当前融合度数顺序 |

- `mean-time-us`：三轮 `elapsed_ns` 之和 / 三轮 shots 之和 / 1000。
- `mean-remaining-syndromes`：未匹配 seed 总数 / 统计 shots，X/Z 两组件相加。
- `matched-percent`：`100 × (初始规范化 seed 总数 − 剩余总数) / 初始规范化 seed 总数`。
  分母为同一设置的 None 初始计数，先验证各策略的规范化输入总数相等。
  这与已匹配 seed 数（`2×pairs + boundaries`）的原生计数交叉核验；不使用整 shot 完成率。
- `xeon.predecoder-comparison.p0p3.degree-ordered.matched-percent.min/max`：七个设置未舍入百分比的最小/最大值，供正文复用。

中间运算使用 60 位 Decimal 精度，最后统一 `ROUND_HALF_UP` 到两位小数；
R 从原始整数计算，不用表格中已经舍入的 N 再算百分比。
表中 56 项指标和正文两项范围边界均来自脚本，正文的 d33 示例复用表中同一组键。
固定工作负载 p、d、测量轮数和单位仍保留为 LaTeX 字面量。

最新 d33 显示值为：None `382.49 / 1929.86`，Simple `223.65 / 161.61 / 91.63`，
Degree-ordered `156.51 / 100.45 / 94.79`；度数策略全范围为 `94.79%–95.18%`。
