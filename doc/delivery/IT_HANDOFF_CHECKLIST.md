# HPU 应用包 IT 交接清单

本文用于交接 `hpu_fhe_delivery` 生成的 CKKS、BFV、BGV application package v1。
包内已经包含指令、resolved DMA、初始 HPU_MEM 镜像、逐算子 golden 和软件验证报告；
RISC-V 编译、Nexus-AM 接入以及 RTL/板级执行由 IT 环境完成。

## 1. 当前交付判定

满足以下条件时，应用包可以交给 IT 开展功能联调：

- 包来自一个明确且干净的源码 commit；
- `hpu_validate_package` 对每个包返回成功；
- `oracle/report.json` 的 `overall_status` 为 `pass`；
- `seal_oracle_to_golden` 和 `host_software_model_to_oracle` 都是必选且为 `pass`；
- 包目录与归档文件的 SHA-256 随交付记录保存。

`instruction_execution_verified`、`rtl_verified` 和 `hardware_verified` 在发布方生成的包中
固定为 `false`。这些字段说明包尚未取得目标执行证据，不影响 IT 开始接入。

Evaluator 和现有 Plan 导出的包都包含 `host_instruction_model` 和 `compiler_resources`。
`compiler_resources.ir_source=operator` 表示资源 IR 由算子直接生成。
默认编译采用 `natural_coefficient_boundaries`，要求 host 逐指令结果与 golden 一致；
正常生成使用 `hpu_frontend_delivery`。显式诊断目标
`hpu_frontend_diagnostic_delivery` 才允许导出带指令模型差异记录的联调包。
诊断包的 `overall_status=pass` 表示必选的 SEAL 与算子软件模型检查通过，
不能据此声明 host 指令模型通过。接收时同时记录
`host_instruction_model_verified` 和 `optimization_equivalence_verified`。
前端的命令及当前验证边界见 [Evaluator 前端入门](../getting-started/HPU_EVALUATOR_FRONTEND.md)。

新应用 builder 为三个方案统一保存自然系数位置。完整变换的软件契约见
[编程手册](../reference/HPU_PROGRAMMING_MANUAL.md#5-变换指令)，表 ABI 和迁移
要求见 [应用包 V1](HPU_APPLICATION_PACKAGE_V1.md#新应用镜像的-ntt-表与软件边界)。
该契约不声称复现 RTL 每 stage 地址。接收时记录
`forward_boundary_conversions`、`inverse_boundary_conversions` 及
`hardware_contract_verified=false`。

`legacy_raw_stage` 用于回放历史 raw 镜像。选择 profile 不修改镜像或指令；
新自然镜像使用 legacy profile 可能比较失败。
`test/reference/main.cpp` 的历史固定 profile 算子包仍是 BR 系数镜像，不能套用
新应用镜像的自然位置表约定。

新包的数据 ABI 位于 `metadata/parameters.json.ntt_table_abi`，它描述镜像而非
回放 profile。缺少该字段的旧包不能仅凭 schema v1 判断为自然表；迁移时重新生成
完整包，不手工补字段替代数据转换。字段明细见 [应用包 V1](HPU_APPLICATION_PACKAGE_V1.md#新应用镜像的-ntt-表与软件边界)。

当前 NTT 数据 ABI version=2，stage 参数格式为 `hpu-twiddle-k8-v1`、K=8。
每个 PNTT/PINTT 的 src2 DLOAD 必须为 **2 line / 512 B**，包括 N=128 和
PINTT stage 0；pre/post PMUL 向量仍为 N 个 word。旧完整表镜像不能直接交给
在线生成 RTL，必须连同 spans、window、manifest 和校验值重新生成。
物理普通 bank 的分配粒度继续由目标 allocator 决定。

硬件组同时记录了 HpuCmdMerge 背压越序的集成问题：仅更新 twiddle RTL 提交
不代表未节流 IT 已消除死锁。目标验收需单独确认 Chisel 合流器修复与完整 IT
结果；本次编译器不新增每 stage 软件等待。详见
[迁移说明第 8.1 节](../reference/TWIDDLE_COMPILER_MIGRATION.md#81-已知硬件合流器问题与参数迁移分开验收)。

## 2. 发布方生成步骤

从干净工作区重新配置构建目录。CMake 在配置时记录源码 commit 和工作区状态，因此
不能复用记录了旧 commit 或 `dirty-at-configure` 的配置结果。

```bash
git status --porcelain
cmake -S . -B build-delivery \
  -DHPU_ENABLE_SEAL_INTEGRATION=ON \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-delivery -j --target hpu_fhe_delivery
# 如交接 Evaluator 前端包，使用默认严格目标
cmake --build build-delivery -j2 --target hpu_frontend_delivery
ctest --test-dir build-delivery -L application-delivery --output-on-failure
```

逐包执行静态校验：

```bash
for package in \
  ckks_polynomial_x2_plus_one \
  ckks_composed_application \
  bfv_multiply_modswitch_application \
  bfv_rotation_application \
  bgv_plain_chain \
  bgv_rotate_chain \
  bgv_multiply_chain \
  bgv_composed_application
do
  ./build-delivery/hpu_validate_package "outputs/${package}"
done
```

交付时整体归档八个默认目录并计算 SHA-256。归档名称应包含源码 commit：

```bash
revision=$(git rev-parse --short=12 HEAD)
tar -C outputs -czf "hpu-applications-${revision}.tar.gz" \
  ckks_polynomial_x2_plus_one \
  ckks_composed_application \
  bfv_multiply_modswitch_application \
  bfv_rotation_application \
  bgv_plain_chain bgv_rotate_chain bgv_multiply_chain bgv_composed_application
sha256sum "hpu-applications-${revision}.tar.gz" \
  > "hpu-applications-${revision}.tar.gz.sha256"
```

## 3. 发布前机器检查

对每个目录检查以下字段：

```text
package.json.schema                  = hpu-application-package
package.json.schema_version          = 1
provenance/build.json.commit         = 本次源码 commit
provenance/build.json.worktree_state = clean-at-configure
oracle/report.json.overall_status    = pass
```

`provenance/files.csv` 的 FNV-1a64 用于包内逐文件损坏检查；交付归档的 SHA-256 用于
确认传输前后拿到的是同一个归档。

### 3.1 目标 CPU 包装代码检查

host 指令模型直接使用 resolved DMA span，不执行 C wrapper。要检查 CPU 编译后
每条 DMA 实际使用的 `x10/x11`，配置支持 RV32 和 RV64 的 RISC-V GCC：

```bash
cmake -S . -B build-delivery \
  -DHPU_ENABLE_SEAL_INTEGRATION=ON \
  -DHPU_RISCV_C_COMPILER=/path/to/riscv64-unknown-elf-gcc \
  -DHPU_RISCV_OBJDUMP=/path/to/riscv64-unknown-elf-objdump
cmake --build build-delivery --target hpu_fixed_executable_test
ctest --test-dir build-delivery \
  -R '^hpu_(fixed_executable|target_dma_codegen)_test$' --output-on-failure
```

`hpu_fixed_executable_test` 检查固定入口生成契约及非法 span/形状拒绝行为。
`hpu_target_dma_codegen_test` 对测试夹具执行 RV32/RV64、`-O0/-O2` 编译，并运行
CPU 包装代码，核对 HPU 指令次序、每条 DMA 的 offset/count、`x11` 设置次数及
寄存器保护。固定入口要求每条 DMA 设置 `x10`，`x11` 只在首次和长度变化时设置。
objdump 为可选项，用于保存反汇编；未找到编译器时，目标测试返回 77，CTest
显示 `Skipped`，此时没有目标代码验证结果。

对实际交付包可直接执行脚本，重复 `--package` 检查多个目录：

```bash
python3 test/target_dma_codegen_test.py \
  --cc /path/to/riscv64-unknown-elf-gcc \
  --objdump /path/to/riscv64-unknown-elf-objdump \
  --package outputs/ckks_evaluator_application \
  --package outputs/bfv_evaluator_application \
  --package outputs/bgv_evaluator_application \
  --output-dir build-delivery/target-dma-check
```

此检查将自定义指令作为检查点记录，HPU 数学结果仍由指令模型和设备读回验证。
保存生成的 ELF、反汇编和检查日志，作为所用目标工具链的代码生成证据。
它不设置包内 RTL/硬件验证状态，也不替代下面的设备验收。

## 4. IT 消费顺序

1. 先解析 `package.json`，使用其中的路径定位文件，不拼接固定子目录。
2. 按 `memory/hpu_mem_config.json` 的 `capacity_lines * 256` 分配并配置 HPU_MEM window。
3. 清零 window，再把 `memory/hpu_mem_image.u32.bin` 从 line 0 开始加载。
4. 将 `program/<stem>.c`、`program/<stem>.h` 编入 RISC-V/Nexus-AM testcase，调用
   `hpu_run_<stem>()`。生成 C 在非 RISC-V host 上不会发射 `.word` 指令，因此 host
   返回值不能作为 HPU 执行通过证据。
5. 执行前完成 cache clean；等待完成 IRQ，记录 `HPU_STATUS` 和
   `HPU_FAULT_STATUS`；读回前执行 cache invalidate。
6. 按 `golden/golden_manifest.csv` 逐条读取目标 line span。比较有效 word，并检查
   padding、未使用区域或外部 guard 没有被越界写入。
7. 先用 golden 条目的 `object_id` 匹配计算图节点的 `golden_object_id`，再用
   节点的 `operation_index`、`id` 匹配 DMA 清单，定位算子、DMA 和对象槽位。
   旧 v1 包若没有显式 `operation_index`，使用 `operations` 数组的零起始下标；
   公共模表 DMA 使用 `$application`，不对应具体算子。

## 5. IT 应回传的签字证据

每个执行案例至少记录：

- 交付归档 SHA-256、Inline-asm commit、Nexus-AM commit 和 RISC-V 工具链版本；
- RTL/FPGA/芯片版本、HPU_MEM 基址与容量；
- testcase 返回值、完成 IRQ、最终 status/fault；
- 每个 golden object 的比较结果和首个失败位置；
- window guard 或等价的越界检查结果；
- 目标侧日志，以及需要时的 DMA/指令 monitor 或波形。

上述证据全部通过后，才能在 IT 的执行报告中声明 instruction、RTL 或 hardware
验证通过。发布方的原始包和 `oracle/report.json` 应保持不变。

## 6. 当前参数范围

| 案例 | N | 用途 |
| --- | ---: | --- |
| `ckks_polynomial_x2_plus_one` | 65536 | CKKS 部署规模功能包 |
| 其余七个默认案例 | 128 | 快速算子、分支和组合流程联调 |
| `*_n65536` 七个部署配置 | 65536 | CKKS 复合应用、BFV/BGV 算子及分支组合验收 |

这些都是功能测试参数，没有安全等级声明。部署规模生成：

```bash
cmake --build build-delivery -j2 --target hpu_fhe_deployment_delivery
for package in outputs/*_n65536; do
  ./build-delivery/hpu_validate_package "$package"
done
```

N=65536 配置也必须完成 oracle 对拍、包校验和目标执行证据。按各包的
`capacity_lines` 配置内存与目标超时；当前示例会把容量收紧到实际 `used_lines`，
不再要求目标分配构建时的预留上限。软件生成成功仍不代表平台已支持相应窗口。
部署包可以单独归档，也可以加入默认八包归档；保存归档 SHA-256 和完整案例清单。
新自然边界 profile 的 N=65536 结果以当次测试与包报告为准，本文列出生成入口和
验收要求，不将历史 raw profile 的通过结果视为新 profile 已通过。
