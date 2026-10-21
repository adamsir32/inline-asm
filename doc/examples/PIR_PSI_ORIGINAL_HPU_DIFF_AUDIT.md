# PIR/PSI 应用恢复原始 HPU 公共代码审计报告

> 历史报告：记录 `51b8ee1` 清理时的状态。后续上游同步按用户要求删除了早期
> Poseidon PIR/PSI 简化示例；当前状态以 `PIR_PSI_UPSTREAM_SYNC_AUDIT.md` 为准。

## 结论

**是。与 `ca0e3dc5118ea4b4c5a996eb6907518f8b4ccde9` 相比，当前树的所有公共
`src/`、`include/` 代码已经恢复原状；保留的 PIR/PSI 应用只调用该基线已有的 HPU
公共接口。** 这一结论同时由 Git 路径 diff、逐文件 blob SHA、全量软件回归和应用包验证
支持。它不表示真实 HPU/RTL 已经运行。

- 原始基线：`ca0e3dc5118ea4b4c5a996eb6907518f8b4ccde9`
- 清理前提交：`3bcfac3721ce3b369441545f3083e1a06bfff1d8`
- 最终清理提交：本报告所在提交；交付后可用 `git rev-parse HEAD` 获得不可自引用的提交 SHA
- 恢复引用：`backup/pir-psi-pre-cleanup-3bcfac3`
- 目标分支：`feature/operator-resource-management`

用户在清理期间明确要求删除二维 SealPIR 的 95 包历史版本。因此本次最终树与最初任务中
“保留 95 包版”的条目存在一项有意偏离：删除其独立入口、CMake 目标、分片评估脚本和
旧生成目录，只保留二维集中式 2 包版。当时仍保留的早期 Poseidon PIR/PSI 简化示例，
已在后续上游同步中按用户要求删除；APSI 集中版和原作者 CKKS/BFV/BGV 示例不受影响。

## 恢复的历史公共改动

本次只在公共目录中做“恢复基线”的反向修改，没有新增公共功能。清理内容包括：

1. 删除 PIR 专用 `Evaluator::apply_galois` 公共接口及相应 OperationPlan、Codegen、
   Relocation、DMA、应用交付和软件执行器路径。
2. 删除 `CompiledApplication::software_result()` 公共接口。
3. 删除 `BfvContextSpec::random_seed` 及 `create_bfv_context()` 的种子扩展。
4. 清理三个通用测试中只为这些扩展增加的用例，同时保留基线原有测试。
5. 没有重新引入 APSI 曾使用的三分量 Add/Sub、专用代码生成或专用执行路径；该部分已在
   `3bcfac3` 完成应用层受限重构。

## ApplyGalois 的应用层替代

二维 SealPIR 查询展开仍执行相同 BFV 多项式自同构。应用层求解离散对数

`element = 3^steps mod (2N)`

后调用基线已有的 `Evaluator::rotate_rows(input, steps, galois_keys, output)`。N=4096 时实际
使用的映射为：

| Galois element | RotateRows steps |
| ---: | ---: |
| 4097 | 1024 |
| 2049 | 512 |
| 1025 | 1280 |
| 513 | 640 |
| 257 | 1344 |
| 129 | 672 |
| 65 | 1872 |

应用使用同一组 SEAL GaloisKeys，且每层展开均由独立 SEAL oracle 与 HPU 软件执行器进行
RNS 系数级对拍。替换后不需要新算子类型；图中的相关节点由历史 `ApplyGalois` 名称恢复为
基线已有的 `RotateRows` 路径。完整规模最终仍为 2 个应用包，当前实测指令与 DMA 数据见下表。

## 阶段边界与随机种子

二维 PIR 第一阶段和 APSI 查询包编译完成后，代码先强制检查：

```cpp
if (!compiled.verified() || !compiled.instruction_verified()) {
    throw std::runtime_error("HPU application verification failed");
}
const auto &result = compiled.oracle_result(value);
```

只有逐节点 RNS 系数对拍和编码指令模型验证都通过，才允许继续 CPU 分解或客户端解密。
准确表述为：**使用与 HPU 软件执行结果逐系数验证一致的 SEAL oracle 密文进行后续 CPU
处理。** 这不是提前跳过 HPU 计算图，也不是从真实硬件读取结果；任何验证失败都会终止，
不会回退到 oracle。

二维 PIR 的可复现种子改由应用直接构造 `seal::EncryptionParameters`、
`seal::Blake2xbPRNGFactory` 和 `seal::SEALContext`，再把合法 context 传给原有
`ApplicationBuilder`。公共 `BfvContextSpec` 无需、也不再包含随机种子字段。

## APSI 两分量路径

APSI Paterson–Stockmeyer 求值中，每次密文乘法后立即调用基线已有的 `relinearize`，将三分量
结果恢复为两分量后才参与普通 Add 归约。small 使用 `t=65537`，medium 使用 `t=270337`；
两者均由应用层 SEAL 参数对象构造。PowersDag、密文乘法、重线性化、MultiplyPlain、Add、
ModSwitch、普通及 Paterson–Stockmeyer 求值仍在 HPU 图中，没有用 CPU oracle 预计算替代。

## 公共代码 Git 证明

以下两条命令在最终工作树和提交后均应分别返回退出码 0、无文件输出：

```bash
git diff --exit-code \
  ca0e3dc5118ea4b4c5a996eb6907518f8b4ccde9..HEAD -- \
  src/hpu src/scheme include/hpu include/scheme

git diff --name-status \
  ca0e3dc5118ea4b4c5a996eb6907518f8b4ccde9..HEAD -- \
  src include
```

清理前曾被应用开发修改的所有公共文件，基线 blob 与清理后 blob 如下：

| 文件 | `ca0e3dc` blob | 清理后 blob | 一致 |
| --- | --- | --- | --- |
| `include/hpu/seal/bfv_context.hpp` | `bd2af09f5b7937c12b3a087dd99ec6a755b1d486` | `bd2af09f5b7937c12b3a087dd99ec6a755b1d486` | 是 |
| `include/hpu/seal/bfv_operation_plan.hpp` | `ff2bd52c8bb120063713effb9c44b59b5c49f200` | `ff2bd52c8bb120063713effb9c44b59b5c49f200` | 是 |
| `include/hpu/seal/bfv_software_executor.hpp` | `4526dff39454b48f39af3539d2fe344b45c46057` | `4526dff39454b48f39af3539d2fe344b45c46057` | 是 |
| `include/hpu/seal/evaluator.hpp` | `bb53888d5eb488678e5bf5caf8f8ec1c83c5baa2` | `bb53888d5eb488678e5bf5caf8f8ec1c83c5baa2` | 是 |
| `src/hpu/seal/application_delivery.cpp` | `19a086f604e9c2f411f88d9bed64f4066b47929e` | `19a086f604e9c2f411f88d9bed64f4066b47929e` | 是 |
| `src/hpu/seal/bfv_context.cpp` | `0b4d76df68cf493f8770efb0457bf9d0274a3c0f` | `0b4d76df68cf493f8770efb0457bf9d0274a3c0f` | 是 |
| `src/hpu/seal/bfv_operation_codegen.cpp` | `b6370041c242cac75d77ad5231960443e3b3edce` | `b6370041c242cac75d77ad5231960443e3b3edce` | 是 |
| `src/hpu/seal/bfv_operation_plan.cpp` | `417347fc387ae353e8b0dd7c6d12c974141da9a7` | `417347fc387ae353e8b0dd7c6d12c974141da9a7` | 是 |
| `src/hpu/seal/bfv_operation_relocation.cpp` | `59284740ad3250be974a5fe88541f28c571aef6e` | `59284740ad3250be974a5fe88541f28c571aef6e` | 是 |
| `src/hpu/seal/bfv_software_executor.cpp` | `e587655d918b3eb4d8602ff10d17f0a2acbfb76c` | `e587655d918b3eb4d8602ff10d17f0a2acbfb76c` | 是 |
| `src/hpu/seal/evaluator.cpp` | `b45d3e04cae29d94f255752b97b7a72284fc6176` | `b45d3e04cae29d94f255752b97b7a72284fc6176` | 是 |

`test/seal_bfv_context_test.cpp`、`test/seal_evaluator_test.cpp` 和
`test/seal_evaluator_coverage_test.cpp` 相对 `ca0e3dc` 也均为零差异。

## 实际验证与资源数据

测试环境为 Ubuntu Release 构建，启用 SEAL integration 和 BUILD_TESTING。最终形态的全量
CTest 为 **84/84 PASS**，0 failed，墙钟 140.43 秒。它在当时覆盖早期 Poseidon PIR/PSI、
二维集中式 small 三个位置（0、64、127）、二维 small 两包交付、APSI small 的 empty、
single、multi、boundary、APSI small 交付，以及全部既有通用回归。所有交付测试都运行
`hpu_validate_package`。

| 应用 | 包数 | 图节点 | 编码指令 | DMA span | 最大 HPU_MEM 行 | 最小噪声 | 结果 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| SealPIR small 128×1024 | 2 | 50 + 82 | 17,725 + 26,741 | 6,434 + 9,642 | 56,835 | 33/33 bit | 记录 0/64/127 PASS |
| SealPIR full 65536×1024 | 2 | 15,172 + 1,907 | 4,265,051 + 637,283 | 1,505,094 + 228,615 | 7,280,771 | 27/28 bit | 记录 65535 PASS |
| APSI small 10/5 multi | 1 | 244 | 452,775 | 179,317 | 303,110 | 7 bit | 交集 PASS |
| APSI medium 8192/512 | 1 | 292 | 301,321 | 118,953 | 179,976 | 5 bit | 32 元素交集 PASS |

SealPIR full 的生成、软件验证、最终恢复和逐包校验耗时 5:46.42，主机峰值 RSS
27,252,252 KiB，产物约 5.5 GiB。APSI medium 的图编译及软件验证为 4.679 秒，交付进程
墙钟 7.094 秒，峰值 RSS 1,446,224 KiB，产物约 190 MiB。APSI medium 实际使用
Paterson–Stockmeyer，配置/观测最高多项式次数为 125/28。

清理前后的集中式包数量不变：SealPIR 仍为每递归阶段一包、合计 2 包；APSI 每次查询仍为
1 包。恢复公共接口没有删减图中的同态步骤。95 包历史版本在用户明确要求后删除；删除前曾
完成软件验证，但它不属于当前树、当前 CTest 或当前 ST 交付，不能用作当前版本通过项。

## 验证边界与残留检查

- 已运行：SEAL oracle 逐节点 RNS 对拍、HPU 软件执行器、编码指令模型、最终记录/交集检查、
  `hpu_validate_package`、84 项全量 CTest、SealPIR full 和 APSI medium。
- 未运行：真实 HPU 板卡、RTL 仿真、IRQ/FAULT/周期与带宽采集；原因是本次仅有 Ubuntu
  主机软件环境。因此 `real_hardware_verified` 保持 `false`。
- 应用中不存在 `software_result()`、HPU frontend `apply_galois()` 或公共 context 随机种子
  扩展依赖。搜索仍可见的 `apply_galois` 是基线已有的 SEAL CKKS 测试或编码自测辅助函数，
  不是 PIR/PSI 新增公共接口。
- 生成的目录包含汇编/编码指令、DMA relocation、初始 HPU_MEM、golden、计算图、资源报告、
  集合或记录测试数据及 `pipeline_manifest.json`。这些是后续 ST 输入，不是硬件 ST PASS。
