# PIR/PSI 非侵入式重构设计与实现

此文描述 `feature/pir-psi-non-invasive-refactor` 相对 inline-asm 审计基线 `bc2c40d` 的应用边界。`include/` 与 `src/` 直接恢复原作者固定提交 `cfee951e`，不修改 BFV 算术、资源调度、ISA、DMA 或应用包 v1 格式。

## 第一阶段完整执行后内存

PIR 应用仍用 `ApplicationBuilder::compile` 构造和严格验证第一维图，再通过公开 `CompiledApplication::package()` 取得真实编码 `program.instructions`、`ordered_dma_spans`、`initial_image` 和所有 `VerifiedOutputLimb`。`write_executed_image` 独立调用公开 `hpu::model::execute_instructions`；仅在 `report.completed()` 且返回字数等于 `used_lines×64` 后，使用 `compare_instruction_memory` 对**全部** Golden 输出逐字核验，然后将 `InstructionExecutionResult.words` 全量写出。它没有读取 SEAL 输出填充内存，也没有用 Golden 覆盖非输出区。现有 `import_stage0_readback` 随后对 16 个模数分量及 SEAL 参考逐系数比较，再产生 32 个 CPU 明文。

该应用层额外重放会增加软件交付的执行时间；相比在编译器内缓存最终内存，其输入机器字/初始镜像/DMA 相同，重构前后完整内存 SHA-256 对照是验收条件。

## 第二维明文到物理分配

`examples/pir_physical_binding.hpp::unique_plaintext_limb` 接收原始逻辑明文经公开 `bfv_multiply_plaintext_to_hpu()` 编码后的单个 Q limb，在公开 `HpuMemImage::allocations()` 中查找。候选必须同时满足：`AllocationKind::plaintext`、只读、完整 `4096` 字、ID 的 `/modN` 后缀与编码模数一致、范围在镜像内，且整个编码 payload 与初始镜像逐字相同。候选恰好一个才建立绑定；零个或多个都失败。它不从 `plain_0/plain_1` 序号推断逻辑身份。

对 32 个逻辑明文各 3 个模数分量生成 96 行 `dynamic_inputs.tsv`，要求 ID 唯一、物理行不重叠、全部编码旧值与镜像相同，并要求镜像恰有 102 个明文分量，留下 6 个固定分量不修改。替换值按同一模数/长度编码；`stage1_patch.py` 仍执行覆盖与非规范系数拒绝。双输入重放沿用**同一套第二阶段机器指令**，再以 SEAL 参考及最终记录比较。Nexus-AM 接收侧既有显式映射校验、CPU 编码和动态输入写保护继续使用，不必更改该仓库。

两个逻辑明文若编码内容相同，同模数的两个物理候选可能同时匹配。仅从包的公开内容无法确定哪一个代表哪个逻辑输入，本实现**明确拒绝**，错误包含 `ambiguous plaintext physical binding`，不会任意选择。`test/pir_physical_binding_test.cpp` 验证正常匹配、错误模数/长度和重复内容拒绝。固定交付测试的 102 个明文分量内容彼此不同；若未来真实工作负载触发此拒绝，可增加独立的受控差分编译/唯一标签辅助清单，并严格对照原程序的指令、DMA、分配地址后再扩大支持。当前不将未经验证的差分编译作为可用能力。

## 来源保护与代码组织

`tools/verified_pir_psi_delivery.py` 是 PIR/PSI 正式交付入口。它要求完整预期提交、干净 Git 工作树、新的构建目录与输出目录；脚本亲自从该源码目录配置并构建应用和校验器，核对 CMake 的源路径及编译定义、应用二进制内嵌提交、构建前后 HEAD/工作树状态；固定原作者可覆盖的交付环境变量，运行 PIR 或 PSI 应用，静态验证每一个包并复核其 `provenance/build.json`，最后才原子发布并写入带工具/二进制 SHA-256 的交付收据。旧构建目录、错误 HEAD、脏源码和错误来源包由新 Python 负例测试覆盖。

**保护范围区别。** 恢复后的原作者通用 `write_application_package` 可以被别的程序直接调用，外部脚本不再为所有通用调用者提供内核级来源拒绝。PIR/PSI 的正式发布流程必须经过该受控入口；Nexus-AM `build-pir-psi.py --producer-commit <完整SHA>` 继续在接收时检查。这一界限不能被表述为全局来源验证完全等价。

新增代码仅在 `examples/`、`test/`、`tools/` 与文档；CMake 只为新测试/目标及移除无用 source-dir 宏作必要调整。原有包结构和目标 CPU ABI 不变。
