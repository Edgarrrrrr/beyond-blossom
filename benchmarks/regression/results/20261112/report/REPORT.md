# Beyond Blossom：代码迁移与性能复核

本机结构化矩阵的126个官方比较点均更快；与同名同线程的来源版相比，34个点耗时增加，最大为3.426%。全部比较共享保存输入，并保留逐配置结果。

打开 [完整离线报告](index.html) 阅读从 summary.json 生成的结果表、正负案例与方法说明。

- [唯一数值来源](summary.json)
- [验证摘要](validation.json)
- [源码对照](code-diff/index.html)
- [三个采用机制与一个未采纳变体](optimization-coverage.json)
- [复现说明](REPRODUCE.md)

实现：edd29b6780713dc78f6a99d67c4c4d192e9c52b5。官方基线：e27b8c6a5f5ba10fb74d4ebb29822f2df5e12bcd。迁移来源：241160314087e5ae698c6e76c3c055a11c3ad422。

后续五臂隔离验证保留为独立测量窗口：120次调用、8次进程重复、30,000条保存记录。单函数noinline变体未采纳；原819-call矩阵不变。见[后续结果](index.html#boundary-followup)、[交互机制](mechanisms/boundary-outline.html)与[完整变体差异](boundary-diff/index.html)。

[后续动态诊断](evidence/dynamic-diagnosis/README.md)与[最终 CI 结果](evidence/ci-final/README.md)保留新增证据，原性能表不变。
