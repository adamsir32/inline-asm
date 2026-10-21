# PIR/PSI 上游同步与验证审计

## 1. 审计范围

本报告记录将 `adamsir32/inline-asm:feature/operator-resource-management` 从原作者上游
`cuihu2/inline-asm:feature/operator-resource-management` 同步到 `7306d1c` 后，对两个
应用层实现进行的兼容性检查、构建、软件验证和交付包验证。同步前 fork 端提交为
`51b8ee1`。

同步采用正常的双亲 merge；未重写上游历史，也未从 `main` 取代码。合入的三个上游提交是：

- `a19197e`：资源调度改用 use index，并引入 SSA operator IR；
- `24e6d41`：固定 DMA 序列复用 `x11`；
- `7306d1c`：twiddle 在线生成并适配硬件版本。

merge 无文本冲突。两个保留应用不需要修改算法或 API 调用；修改仅限清理过时应用入口、
构建配置和文档中的失效引用。

## 2. 最终应用清单

相对于应用开发前共同基点 `ca0e3dc`，`examples/` 中由本工作新增并在最终树中存在的 C++
应用实现只有：

```text
examples/bfv_sealpir_2d_application.cpp
examples/bfv_apsi_concentrated_application.cpp
```

已删除且未恢复：

- `examples/bfv_poseidon_pir_application.cpp`；
- `examples/bfv_poseidon_psi_application.cpp`；
- SealPIR 95 包历史实现及其专属入口。

原作者已有 CKKS、BFV、BGV 示例和测试均保留。CMake 中也只保留当前二维 SealPIR 与
集中式 APSI 的新增应用目标；旧 Poseidon 目标、测试注册和专属说明已清理。

## 3. 公共 HPU 代码零差异证明

最终树对实际同步的上游提交执行：

```bash
git diff --exit-code 7306d1c69010b3e92576a19f1d90b1c25cf01f8c..HEAD \
  -- src include encode
```

命令退出码为 0、无输出。因此 `src/`、`include/`、`encode/` 与原作者上游逐字节一致，
本次没有添加 PSI/PIR 专用底层算子、Evaluator API、OperationPlan 类型、Codegen、DMA、
Relocation、资源管理或软件执行路径。

可使用下面的逐对象命令再次核验任一关键文件；左右 Git blob SHA 必须相同：

```bash
for f in \
  encode/include/executable.hpp \
  encode/src/executable.cpp \
  include/hpu/compiler/operator_resources.hpp \
  include/hpu/seal/detail/stage_twiddle_memory.hpp \
  src/hpu/compiler/operator_resources.cpp \
  src/hpu/compiler/resource_ir.cpp \
  src/hpu/delivery/resource_compiler.cpp \
  src/hpu/model/hardware_ntt.cpp \
  src/hpu/seal/bfv_operation_plan.cpp \
  src/hpu/seal/bfv_operation_relocation.cpp \
  src/hpu/seal/bfv_software_executor.cpp; do
  test "$(git rev-parse 7306d1c:$f)" = "$(git rev-parse HEAD:$f)" || exit 1
done
```

两个应用源文件相对同步前 `51b8ee1` 也无差异，说明上游适配没有改变 PIR/PSI 数学语义：

```bash
git diff --exit-code 51b8ee1..HEAD -- \
  examples/bfv_sealpir_2d_application.cpp \
  examples/bfv_apsi_concentrated_application.cpp
```

## 4. 构建与原作者回归

服务器环境为 Ubuntu 24.04，Release 构建启用 SEAL 集成：

```bash
cmake -S . -B build-upstream-sync \
  -DHPU_ENABLE_SEAL_INTEGRATION=ON \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-upstream-sync -j2
ctest --test-dir build-upstream-sync --output-on-failure -j2
```

完整构建通过，用时 1:32.62，峰值 RSS 577,648 KiB。CTest 为 **0 FAIL**：84 个已注册
测试中 83 个实际执行并通过；`hpu_target_dma_codegen_test` 因环境中没有同时支持 RV32
和 RV64 的 RISC-V GCC，被测试自身按设计标为 SKIP。整个 CTest 用时 81.75 秒，峰值
RSS 2,655,472 KiB。这个环境性 SKIP 不伪装成 PASS。

## 5. SealPIR 验证

小规模 128×1024-byte、二维递归用例验证记录 0、64、127，全部通过。独立 small 交付目标
和两个包的 `hpu_validate_package` 通过。完整规模保持 Microsoft SealPIR 配置：
65536×1024-byte、`N=4096`、`logt=20`、`d=2`、86×85，检索记录 65535。

完整规模实际完成第一维 HPU 结果、CPU 密文分解、第二维 HPU 输入、客户端重组与最终
1024-byte 记录恢复；没有以 SEAL oracle 密文替代跨阶段数据。

| profile | 包 | graph nodes | instructions | DMA spans | 最大 HPU_MEM 行 | 最小 noise | 结果 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| small | 2 | 50 + 82 | 17,725 + 26,741 | 6,434 + 9,642 | 47,475 | 33/33 bit | 0/64/127 PASS |
| full | 2 | 15,172 + 1,907 | 4,265,051 + 637,283 | 1,505,094 + 228,615 | 7,266,011 | 27/28 bit | 65535 PASS |

full 交付进程墙钟 6:07.71，峰值 RSS 31,080,760 KiB，输出约 5.7 GiB、102,328 个文件。
small 的一次增量交付复跑为 2.09 秒、峰值 RSS 237,356 KiB；该值不是冷启动编译时间。

## 6. APSI-compatible PSI 验证

small 的 empty、single、multi、boundary 四种集合语义均通过。medium 保持 8192 个发送方
元素、512 个接收方元素、32 个交集元素，实际最高 bin 多项式次数为 28，并执行
`ps_low_degree=5` 的 Paterson–Stockmeyer 分支。每个 profile 的全部 bundle 仍集中在一个
HPU 应用包中。

| profile | 包 | graph nodes | instructions | DMA spans | HPU_MEM 行 | noise | 图编译+软件验证 | 交付 wall / peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| small multi | 1 | 244 | 452,775 | 179,317 | 283,766 | 7 bit | 11.749 s | 15.920 s / 2,655,472 KiB |
| medium | 1 | 292 | 301,321 | 118,953 | 171,336 | 5 bit | 5.639 s | 8.379 s / 1,723,756 KiB |

两种 profile 均通过 SEAL oracle、HPU 软件执行器逐 RNS 系数比较、host 编码指令模型、
最终集合交集检查和 `hpu_validate_package`。medium 的 HPU 算子计数为 multiply 30、square 6、
relinearize 36、multiply_plain 55、add 53、add_plain 2、mod_switch_to_next 110。

## 7. 同步前后资源变化

节点数、编码指令、DMA span、包数和噪声预算在同步前后完全相同。变化集中在上游在线
twiddle 生成带来的 HPU_MEM 降低：

| 工作负载 | 同步前 | 同步后 | 变化 |
| --- | ---: | ---: | ---: |
| SealPIR small 最大单包 | 56,835 | 47,475 | -9,360（-16.47%） |
| SealPIR full 最大单包 | 7,280,771 | 7,266,011 | -14,760（-0.20%） |
| APSI small | 303,110 | 283,766 | -19,344（-6.38%） |
| APSI medium | 179,976 | 171,336 | -8,640（-4.80%） |

本次 full PIR 与两个 APSI profile 的墙钟和主机峰值 RSS 高于此前记录；测试期间共享服务器
存在其他高内存/高 CPU 进程，因此只报告原始实测值，不把差异归因于上游改动，也不把
应用包数或 HPU_MEM 减少等同于硬件性能提升。

## 8. 交付目录与验证边界

服务器生成目录：

```text
outputs/bfv_sealpir_2d_concentrated_small/
outputs/bfv_sealpir_2d_concentrated_full/
outputs/bfv_apsi_concentrated_small/
outputs/bfv_apsi_concentrated_medium/
```

目录包含 `pipeline_manifest.json`、CPU/HPU 边界、集合或记录测试数据、HPU assembly、编码
指令、DMA relocation 清单、初始 HPU_MEM、逐输出 golden、计算图及资源/验证报告。

本报告证明的是 SEAL oracle、HPU 软件执行器、host 指令模型与应用包验证器通过。没有真实
HPU 板卡或 RTL 的执行、内存回读、IRQ/FAULT 和外部 monitor 证据，所以所有 manifest 的
`real_hardware_verified` 保持 `false`，不得称为真实硬件 ST 已通过。
