# 2026 年 10 月 NTT 契约与 CKKS 舍入修复审计

审查日期：2026-10-04；自然系数边界修复日期：2026-10-08。
本文保存修复依据和软件验收快照。当前接口以 [编程手册](../reference/HPU_PROGRAMMING_MANUAL.md#5-变换指令)
和 [应用包 V1](../delivery/HPU_APPLICATION_PACKAGE_V1.md#新应用镜像的-ntt-表与软件边界) 为准。
软件逐字对拍不代替 RTL stage 地址或目标执行验收。

## 1. 修复的两个独立问题

旧算子 reference 的系数镜像为 `memory[p]=c[BR(p)]`，pre 表为 `psi^BR(p)`，
post 表按 raw 系数位置准备。BFV 导入桥则直接复制 SEAL 的自然系数；三方案
SoftwareExecutor 通过数学 NTT API 使用自然系数。逐指令模型早期只转换正向
入口，未统一前置 PMUL 的表索引与逆向出口，导致同一应用使用不同边界约定。

N=128、q=65537、psi=282、`c[1]=1` 的独立反例中，正确 negacyclic NTT 的
物理 word 0 为 282，自然系数混用旧 BR pre 表得到 65281。排序后的输出也不同，
因此不能靠最终输出排列修正错误的 twist。真正频域输入经过 raw INTT 和旧 post
得到 BR 系数，公开数学 inverse API 则返回自然系数。

另一个独立问题是 CKKS single-P KeySwitch：SoftwareExecutor 使用中心化 P
余数，旧 codegen 使用通用非舍入 ModDown。当 P 余数超过 P/2，两者的商可能
相差 1，经 NTT 后影响多个 word。此问题需要修复算术，不能归因于系数排列。

SEAL oracle、算子 SoftwareExecutor 和编码指令回放分别执行；算子模型通过
不能单独证明 codegen、relocation 或指令回放正确。优化前后的一致性也与
数学 oracle 对拍分别记录，避免把原始流已有的问题归因于 workspace 复用。

## 2. 独立数学检查与 raw 模型来源

| 来源 | 提供的信息 |
| --- | --- |
| [`hw_ntt_intt_complete.py`](../../../autotest/hw_ntt_intt_complete.py) | 128 寄存器、64 BF lane、固定 loader、P/P^-1、lazy scale；C++ raw 模型的来源 |
| [`intt_twiddle_gen.py`](../../../autotest/intt_twiddle_gen.py) | 逐 stage/batch/lane 的实际 INTT BF 表项、地址和 scale |
| [`visualize_intt_schedule.py`](../../../autotest/visualize_intt_schedule.py) | 默认 dual 调度；旧 post 模式要求正向跳过最后一次 P，不能混用 |
| [`toy_fhe_auto.py`](../../../autotest/toy_fhe_auto.py) | 显式 BR 的数学 NTT、自然 twist/untwist、modified-root automorphism |

raw 正向 stage 执行 `load → BF → P → store`；逆向按反序 stage 执行
`load → P^-1 → BF → store`。P 是局部 7 位寄存器索引 rotate-right，不能等同于
全 N 点 BR。INTT 实际表项为 `w_bf=alpha/beta`，输出 scale 为
`(alpha,alpha*w_forward)`；其表项不等于简单的数学逆 twiddle。
loader 和写回公式见编程手册。complete 模型和独立 twiddle generator 是循环
变换模型，没有执行 negacyclic pre-twist 或 post-untwist。

令 `labels[p]` 为 raw 正向的最终物理标签。直接求和
`sum(c[i]*omega^(i*k)) mod q`，不调用 Python 的 DIT reference，得到：

```text
Python raw_ntt(input)[p] = DFT(BR(input))[labels[p]]
C++ forward(input)[p]   = DFT(input)[labels[p]]
                       = Python raw_ntt(BR(input))[p]
```

在 N=128/256、q=65537 下，未重排输入的 Python 对单项式 X 仅产生 2 个不同
谱值，正确数学 NTT 分别产生 128/256 个值。Python 的逐点平方再 INTT 得到
常数 1，数学循环卷积应得到 X²；直接正反往返仍能通过。这证明往返不能替代
独立正向或卷积 oracle，也不能把输入 BR 错误解释成仅差输出排列。

N=128/256/512/1024/2048、q=12289 的交叉检查还确认 C++ INTT 表与独立
generator 的 batch/lane 表相同，最终 raw 标签恢复 identity，生成的 lazy
scale 最终为 1，post scale 为 N^-1。数学频域输入经 Python raw INTT 返回 BR
系数，C++ 数学接口恢复自然系数。这些检查核对软件语义，不反推真实硬件的自然入口错误。

## 3. CKKS single-P 舍入修复

SEAL single-P、singleton-digit 路径复用
[`rounded_drop_last.cpp`](../../src/operator/rounded_drop_last.cpp) 的
`generate_hpu_rounded_single_p_moddown_contexts_body_asm()`，在全 QP 中准备
half-P 常量并补齐 DMA 绑定。half 加法只写已有可变 accumulator，不增加工作区副本。
Rotate、Conjugate、Relinearize 共用该路径；通用多 P codegen 保留原实现。

回归覆盖 half-P 精确边界、旧非舍入路径的反例，以及 N=128、Q3/Q2 的
重线性化、负向旋转和共轭逐字 SEAL 对拍。常量、指令和 relocation 必须来自同一次
生成，旧应用包需重新生成才能包含修复。

## 4. 自然系数边界与表 ABI

新 BFV、CKKS、BGV ApplicationImage 统一采用：

```text
memory[i] = c[i]
pre_twist[i] = psi^i mod q
post_untwist_scale[i] = raw_inverse.post_scale[BR(i)] * psi^-i mod q
```

raw post scale 包含 lazy scale 抵消和 N^-1。modified-root INTT 使用对应 root
的 raw inverse 表和 modified psi。公共 `create_fused_inverse_automorphism_tables()`
保留 raw post 输出，builder 导入时转换为自然 post。SoftwareExecutor 正向直接
读取 `pre_twist[i]`；逆向通过 `InverseNttTables::set_natural_post_scale()` 适配给
保持 raw post 约定的数学 inverse API。stage 表仍按 loader batch/lane 顺序，不能 BR。

默认 `InstructionNttContract::natural_coefficient_boundaries` 执行：

```text
正向：显式自然 pre-PMUL → stage 0 软件边界 BR → reference raw stages
逆向：reference raw stages → 最后 stage 软件边界 BR → 显式自然 post-PMUL
```

这是完整自然边界的功能软件契约，不声称复现 RTL 每 stage 地址周期。PMUL 逐位置
消费原 DMA 载荷；逆向边界只移动位置，normalization 和 untwist 仍显式执行。
报告记录 `forward_boundary_conversions` 与 `inverse_boundary_conversions`。
`legacy_raw_stage` 保留历史 reference 的 BR 镜像回放，不增加整体边界转换。
选择回放 profile 不修改数据或指令；新自然镜像与 legacy profile 混用可能失败。

`metadata/parameters.json.ntt_table_abi` 版本为 1，系数、pre/post 索引为 natural，
stage 表为 `reference_loader_batch_lane`，NTT 排列为 `reference_forward_layout`，
normalization 为 `explicit_post_pmul`，`twist_fused=false`。此数据 ABI 不随回放
profile 改变；旧包迁移必须重新生成镜像、golden、元数据和校验清单。
跨域 workspace 可保持 `unspecified` 布局标签，不能仅凭名称推测 stage 中间位置。

## 5. 软件验收快照

精简重复诊断测试前，完整构建及 CTest **76/76** 通过，包含独立奇根多项式求值、
schoolbook 负循环卷积、实际编码的 pre/post-PMUL、全部 PNTT/PINTT stage、
DMA、CKKS 舍入回归、三方案非线性前端图和严格交付生成。

后续删除仅正向边界的临时 profile、三个重复诊断成功用例、未使用的 IR 字段和
重复编译收尾检查；合并 raw/model 的 batch 生成逻辑，并移除数学 forward 中未使用
的 labels 维护。数学结果、变换表及交付数据约定不变，三方案 strict gate 保留。
清理后的完整构建成功，CTest **73/73** 通过。

三方案前端按新的默认契约严格生成并通过静态包校验：

| 方案 | N=128 比较 word | N=65536 比较 word | 优化前后输出 | 软件指令对拍 |
| --- | --- | --- | --- | --- |
| CKKS | 5,248 | 2,686,976 | 一致 | 通过 |
| BFV | 5,248 | 2,686,976 | 一致 | 通过 |
| BGV | 5,248 | 2,686,976 | 一致 | 通过 |

原 Plan 示例应用包也重新生成并通过其既有算子软件验证和包校验；没有执行前端
指令验收的包不补写该状态为 true。严格编译同时要求 SEAL、SoftwareExecutor、
指令回放逐字一致，并独立比较原始与优化流。可持续回归入口为
[`instruction_executor_test.cpp`](../../test/instruction_executor_test.cpp)、
[`seal_ckks_keyswitch_instruction_test.cpp`](../../test/seal_ckks_keyswitch_instruction_test.cpp)
及三方案 frontend 交付测试。

## 6. 硬件证据范围

已确认目标行为是自然系数输入、PNTT 内部调度与 shuffle 完成正确变换、输出排列
不同于 SEAL，当前 twist 不融合。软件修复按该目标推进，真实 RTL 地址及时序另行验收。

只读核对的 [PEcore 文档](https://icnj64z5e8zz.feishu.cn/wiki/LurMwL8lcizkxok5eYNcJraLnAc)
说明普通 PMUL lane 一一对应和局部互逆旋转；
[控制逻辑文档 v0.4](https://icnj64z5e8zz.feishu.cn/wiki/KOlSwfEEtiMuqvkTppPcyJElnyf)
说明 stream_ctrl 负责地址、拼包及 out-of-place 写回。
[IT 验证方案](https://icnj64z5e8zz.feishu.cn/wiki/AkquwXJxBiZiRHkdqspc1ab9nyh)
要求单 stage 和整体变换的独立 golden，但要求本身不构成已执行证据。
控制文档的 stage<6 边界与 raw 模型的 stage<7 尚未逐项对应；局部 PE 测试和
random_stress PASS 不能代替完整控制器 NTT 数学闭环。

前端包的 `host_instruction_model.hardware_contract_verified` 保持 false；所有包的
`instruction_execution_verified`、`rtl_verified`、`hardware_verified` 保持 false。
本次工作没有修改飞书、`autotest` 或 `nexus-am`；软件验收不会自动改变目标验证状态。

后续原生算子资源 IR 迁移也通过了 73/73 回归和三方案 N=65536 前端包验收。
现有 Plan 包导出已接入公共资源编译器并做严格 host 指令对拍；上述首次修复时的
Plan 验证范围记录保留为验收快照。当前入口及报告字段见
[编译器资源管理](../architecture/HPU_COMPILER_RESOURCES.md)。
