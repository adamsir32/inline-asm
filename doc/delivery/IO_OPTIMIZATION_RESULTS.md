# SSA 与 IO 优化实测记录

日期：2026-10-23。本记录来自当前工作区的主机运行，产物保留生成时的源码 revision
和工作区状态；正式发布时应从确定的提交重新生成。

## 1. 本轮变更

- 算子以独立 SSA 值、逻辑数据引用和输出声明描述计算；不再手写 PFREE 或决定
  DSTORE REL。输入、常量与 workspace 按语义名称绑定，取消读取序号和固定 DMA
  数量对数学算法的约束。
- 在物化之前执行读取合并、写读转发和输出根分析；随后比较原序、复用及依赖重排
  候选，以最终 DMA 字节、DMA 次数和指令数依次选择。
- 后端按需加载，统一处理最后使用、bank 放置、干净数据释放、脏值 spill/reload，
  并复用临时 DDR。保留实际硬件的 NTT/PMAC 新旧区间共存、bank mask 及 CP 规则。
- Evaluator 和 Plan 交付适配器默认只导出指定结果；没有 mark_output 时保留最后
  一个计算结果。三个复合主示例只标记 y；显式多输出继续支持。
- 原生算子、Plan 与 Evaluator 使用同一应用包编译链。旧直接汇编交付入口退出，
  ISA 编码及兼容导入仅保留作内部测试。Package V1 的目录、二进制及 DMA CSV 格式
  保持不变，前端 Evaluator、句柄、mark_output 和 compile 调用保持不变。

## 2. N8192、Q4、P1 主基准

参数为 8192 个系数、4 个数据 Q 和 1 个 special P，seed=0，只导出 y。表中单位
均为字节，包含 DLOAD 与 DSTORE。原序与优化产物使用同一输入、参数和 oracle
数据指纹；原序也使用统一后端及相同输出根。

| 方案 | 原序 DMA | 优化 DMA | 减少 | 选择的调度 | spill 次数 |
| --- | ---: | ---: | ---: | --- | ---: |
| CKKS | 52,975,872 | 8,975,616 | 83.1% | reordered | 0 |
| BFV | 94,615,296 | 15,615,232 | 83.5% | reordered | 25 |
| BGV | 29,930,752 | 5,748,992 | 80.8% | reuse | 0 |

按最终 memory manifest 的物理数据类别分解减少的 DMA 字节：

| 数据类别 | CKKS | BFV | BGV |
| --- | ---: | ---: | ---: |
| ciphertext | 0 | 622,592 | 0 |
| constant | 2,981,888 | 13,729,792 | 1,114,112 |
| evaluation_key | 0 | 0 | 2,981,888 |
| plaintext | 0 | 0 | 65,536 |
| twiddle | 4,645,888 | 5,435,904 | 3,505,152 |
| workspace | 36,372,480 | 59,211,776 | 16,515,072 |

这些分类可以相加得到总节省量。workspace 的变化主要包含内部写回、重读的消除；
常量、twiddle、密钥的变化包含复用与重排的收益。`classified_io` 另列 exact-copy、
spill 和 reload，属于同一批传输的另一观察维度，不能再与上表相加。

本次三个图的 `output_policy_saved_dma_bytes` 均为 0：单独删除不导出的根、保留
原来的 store/load 依赖时，中间值仍被后续节点读取。进一步通过 SSA 转发才能删除
这些往返，因此收益计入相同输出根的编译优化。`removed_checkpoint_min_write_bytes`
只描述未导出 checkpoint 的最少写回体量，不作为额外、可相加的节省量。

读取合并后，同一个 SSA 值可以在容量压力下合法重载。原序中的重复读取往往是
不同 SSA 定义，因此不能单凭两次编译的 reload 次数判断优劣；应比较实际 DMA
总字节、逐类别及逐地址范围统计。

机器报告：[优化与对照](../../outputs/io_refactor/io_summary.json)、
[原序产物](../../outputs/io_refactor_baseline/io_summary.json)。报告包含逐 SSA 值、
逐 allocation、逐数据类别的读写字节，以及候选调度、初始镜像大小和峰值占用。

## 3. 局部验收

现有 `hpu_native_io_optimization_test` 在 N8192 下验证实际生成指令和数学结果：

| 用例 | 结果 |
| --- | --- |
| CMULT，4 份不同输入，Q4 | 16 次输入 DLOAD、512 KiB 输入读取、无 spill；旧实现为 32 次，减少 512 KiB |
| 同一 pre-twist 用于 6 次 NTT | 因子加载从 6 次降到 1 次，减少 160 KiB |
| 同模数的 3 次 INTT | stage 参数加载从 39 次降到 13 次，减少 13 KiB |
| 内部存储转发与输出后继续使用 | SSA/资源回归验证没有强制释放导致的额外重读 |

CMULT 的旧次数来自重构前的四组两操作数加载；整体基准表使用的是新算子与统一
后端的原序对照，不能将局部 CMULT 节省再次加到整体表中。

原生三个 fixture 使用未加密的环算术参考，不宣称是完整 CKKS 的 SEAL 对拍。
应用包见 `outputs/isa12/native_{cmult,ntt,intt}_n8192/`。
其他算子由 CKKS/BFV/BGV 的 Plan 与 Evaluator 应用包覆盖。历史独立 `.asm/.cpp`
目录生成命令已退出；本轮未为每个历史目录保留一一对应的原生 fixture 命令。

## 4. N65536 容量压力场景

保持 Q4、P1、seed=0。原序数据来自每个包中实际编译的原序候选，优化仍按总 DMA
字节选择；允许必要 spill。

| 方案 | 原序 DMA | 优化 DMA | 减少 | spill 次数 |
| --- | ---: | ---: | ---: | ---: |
| CKKS | 416,776,448 | 210,996,480 | 49.4% | 196 |
| BFV | 747,954,432 | 468,421,376 | 37.4% | 516 |
| BGV | 233,586,944 | 174,765,312 | 25.2% | 150 |

[压力场景机器报告](../../outputs/io_refactor_n65536/io_summary.json)。三个方案均通过
最终指令模型与 SEAL oracle 的输出对照，以及原序/优化产物等价检查。

## 5. 复现与验证边界

```sh
cmake -S . -B build -DHPU_ENABLE_SEAL_INTEGRATION=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
ctest --test-dir build -j6 --output-on-failure

python3 tools/benchmark_io.py --build-dir build --output-root outputs/io_baseline --no-opt
python3 tools/benchmark_io.py --build-dir build --output-root outputs/io_optimized \
  --baseline-json outputs/io_baseline/io_summary.json
python3 tools/benchmark_io.py --build-dir build --output-root outputs/io_n65536 --degree 65536

cmake --build build --target hpu_frontend_delivery hpu_fhe_delivery hpu_native_kernel_delivery -j6
```

本轮最终完整回归为 78/78 通过；不启用 SEAL 集成时，核心 SSA 和资源分配回归
亦通过。全部已生成应用包经过磁盘格式校验。

这些数据衡量生成指令请求的传输量，不等同于实际 DDR 总线流量或执行周期。
CP=1 仍只用于 twiddle 加载，统计没有假定 LLC 命中，也没有修改 Nexus-AM、驱动
或硬件契约。RTL/板端流量和周期仍由后续硬件运行确认。
