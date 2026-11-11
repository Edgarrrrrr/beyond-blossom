# L2/L3 PMU 测试、离线结果与论文宏

本轮归档用户已完成的 `DECODER-L2-CACHE-20261110/formal-r01` 和
`DECODER-L3-CACHE-20261110/formal-r04`，核心实现均为
`eb66e671947d066adafab1b2a504d45b2f0530d7`，T1、LTO OFF。
两个缓存级别各21设置×2入口×1轮百万shots，共84次调用；复用同一2100万独立输入。
范围为独立X/Z、非相关解码，d=9/13/17/21/25/29/33、p=0.05%/0.1%/0.3%、rounds=d。
AE只新增采集入口及这两轮结果，不修改已有`grid`图的历史PMU数据。

## 离线验证与四个宏

```bash
python3 AE/pmu_data.py verify
python3 AE/migrate_paper_numbers.py --check
python3 AE/migrate_paper_numbers.py --output /absolute/path/paper_numbers.tex
python3 -m unittest discover -s AE -p 'test_*.py' -q
```

以上命令只用Python 3.11+标准库，可单独复制AE目录后运行。
数据在`data/pmu/`，L2/L3各有完整`summary.csv`、`summary.json`、冻结计划、退出/完成凭据和42组原始PMU记录。
`SOURCE.json`用相对路径关联每份原始计数、验证、预热、正确性和执行记录，并保存SHA256。
源文件保持原字节；原始绝对路径仅用于溯源，离线宏生成不会访问它们。
百万行逐shot结果、输入和二进制留在服务器原始run；离线核验沿用已完成的逐条校验凭据，不重新解码。

| 宏（前缀均为 `xeon.ablation.d33.p0p3.`） | 数据 | 三位小数 |
| --- | --- | --- |
| `pymatching.L3miss` | 68450499 / 72825538766 × 100 | 0.094 |
| `structured-simd.L3miss` | 3886983 / 23423543446 × 100 | 0.017 |
| `pymatching.L2miss` | 72891746270 / 121057626186 × 100 | 60.212 |
| `structured-simd.L2miss` | 22539919860 / 83518786290 × 100 | 26.988 |

四个数均以**百分数**表示，TeX值不带百分号。宏规则`pmu_miss_percent`与原有Batch3规则分别读取各自来源。
使用整数分子/分母和60位Decimal精度计算，最后按模板位数ROUND_HALF_UP；不把已舍入的百分比作为输入。
既有24项时间/加速宏及5项M4保持原值，M4整个区段保持原字节。
缺失设置、方法、事件、非0退出、计数/校验SHA不符或PMU时间复用均使宏生成失败，不写回部分结果。

L2使用Cascade Lake raw事件0xe124（读取请求）和0x2124（读取miss），按Intel定义分母包含L1D硬件预取器相关请求。
L3使用Linux `LLC-loads` / `LLC-load-misses`成组计数。两者均为`100 × miss请求数 / 读取请求数`，各自分母单列。
实际事件编码及原始组读取保留在每份`pmu.json`，没有从静态汇编推算计数。

## 重新采集（需原服务器、Git checkout及冻结原始矩阵）

`pmu_collect.py`、`pmu_runtime.py`、`pmu_native/`及`pmu.sbatch`构成AE内的完整采集入口。
它重用参考矩阵的原输入和已冻结核心库，只重编译独立PMU适配层；L2/L3由编译宏选择。
先查看`squeue`选择空闲节点；只允许ares1–6，节点独占，每节点同时最多一个任务。
参考run需要包含`plan.json`、准备好的输入/DEM和其声明的核心库/头文件，依赖在冻结计划中逐项记录。

```bash
REFERENCE=/absolute/path/DECODER-DOCUMENT-MATRIX-20261106/formal-r01
RUN=/absolute/new-run/pmu-L2
python3 AE/pmu_collect.py freeze --level L2 --reference-run "$REFERENCE" \
  --nodes ares2 ares3 ares4 ares5 ares6 --output "$RUN" \
  --cache-dir /absolute/new-build-cache/pmu-L2
python3 AE/pmu_collect.py submit --stage build --output "$RUN"
python3 AE/pmu_collect.py submit --stage formal --output "$RUN"
python3 AE/pmu_collect.py submit --stage analysis --output "$RUN"
```

测L3时改为`--level L3`并使用新的run和cache路径。不要让两轮占用同一节点并发测量；`--exclusive`会串行同节点作业。
`build`包含两入口、两端参数的1025条计数与精确结果检查；`formal`依赖build成功，汇总依赖所有采集成功。
原始结果写入`analysis-r01/`，任何输出目录已存在时都拒绝覆盖；失效依赖会取消后续任务。
每进程前1024条循环预热至少2秒，连续解码窗口内不读钟，排除构图、加载、校验及输出；不测延迟。
计数器pinned组不缩放、不时间复用；不支持的事件会报错，不替换成其他缓存级别。

## 导出新一轮数据

```bash
python3 AE/pmu_data.py bundle --l2-run /absolute/completed-L2-run \
  --l3-run /absolute/completed-L3-run --output /absolute/new-pmu-bundle
python3 AE/migrate_paper_numbers.py --pmu-data /absolute/new-pmu-bundle/SOURCE.json \
  --output /absolute/path/new-paper-numbers.tex
```

显式指定的新包仍需满足相同实现、完整矩阵、输入身份、事件和完成/退出条件；audit记录实际采用的包和计数。
正式更新AE默认结果时，在新版本归档数据，更新映射中`pmu_source.sha256`及`data/SHA256SUMS`入口，保留旧run与报告身份。
