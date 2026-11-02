# PIR/PSI 非侵入式改动审计

受保护基线是原作者 inline-asm `cfee951e4056660e521d55a3535d82335a49bb08`；重构前审计版本是 `bc2c40d535413a30a7dfc4bb3323aecda3ca18e2`。调查先执行 `git diff --name-status cfee951e bc2c40d -- include/ src/`，确认恰有以下八个修改文件，没有新增文件。随后才实施迁移。

| 原文件 | 原来增加的内容及依赖者 | 原作者公开替代接口和新位置 |
|---|---|---|
| `include/hpu/delivery/resource_compiler.hpp` | `compile_application_resources(..., final_execution_words)`；由 frontend 包装调用 | 已有 `hpu::model::execute_instructions`；`examples/bfv_sealpir_2d_application.cpp::write_executed_image` |
| `src/hpu/delivery/resource_compiler.cpp` | 保存编码指令执行器最终完整内存 | 应用层在编译完成后使用包中编码机器字、DMA 清单和初始镜像再执行，并逐项比较所有 Golden；不拼接 Golden |
| `src/hpu/seal/frontend_package.cpp` | 转发内存捕获指针 | 不再需要转发；原函数签名恢复 |
| `include/hpu/seal/evaluator.hpp` | `CompileOptions::capture_execution_memory`、两个 `CompiledApplication` getter、额外 include | 应用层重新执行软件模型；物理绑定由 `examples/pir_physical_binding.hpp` 以编码内容和公开分配表确定 |
| `src/hpu/seal/evaluator.cpp` | `execution_memory_words` 缓存、`plaintext_allocation_ids` 内部表及 getter | 移除。应用示例不再访问私有 `plain_N` 创建顺序；相同内容候选明确拒绝 |
| `include/hpu/delivery/application_package.hpp` | `ProducerIdentity` 的内存态源路径/配置状态及 `verified_producer_identity` 声明 | `tools/verified_pir_psi_delivery.py` 独立受控交付流程 |
| `src/hpu/delivery/application_package.cpp` | 生成时 Git HEAD/工作树核对，`write_application_package` 自动调用 | 外部脚本在新目录配置、编译、发布前后检查提交/清洁状态，检查二进制嵌入提交和每个包的来源字段 |
| `src/hpu/seal/application_delivery.cpp` | 生成时调用新增来源函数及 source-dir 宏 | 恢复原作者来源填充；PIR/PSI 正式交付只经受控脚本，并在 Nexus-AM 接收器继续强制完整提交号 |

**其他调用者清理。** `examples/native_kernel_application.cpp` 原来调用新增来源函数，现使用原作者 `ProducerIdentity` 三字段接口；`test/application_package_test.cpp` 原来直接调用核心 guard 的负例迁移为 `test/verified_pir_psi_delivery_guard_test.py`，覆盖旧提交、脏源码、旧构建目录与旧包来源四类拒绝；`CMakeLists.txt` 去掉已无用途的 source-dir 编译宏并注册替代测试。PIR 示例移除两个 getter 调用。历史文档保留原始记录并加当前实现的指引。

**隐式依赖。** CMake 在配置时写入提交/工作树状态，原作者应用侧可接受环境变量覆盖；外部受控交付因此强制新构建目录、校验配置源路径/编译定义/可执行文件中的提交，固定环境变量并复核包内 `provenance/build.json`。直接运行通用包写入器仍只有原作者的保护范围；本次承诺的是 PIR/PSI 正式交付路径，不声称对所有通用调用者提供全局 guard。

**硬性树校验。** 在最终提交上运行：

```sh
git diff --exit-code cfee951e4056660e521d55a3535d82335a49bb08 HEAD -- include/ src/
test "$(git rev-parse cfee951e4056660e521d55a3535d82335a49bb08:include)" = "$(git rev-parse HEAD:include)"
test "$(git rev-parse cfee951e4056660e521d55a3535d82335a49bb08:src)" = "$(git rev-parse HEAD:src)"
git ls-files --others --exclude-standard include/ src/
```

最后一项必须为空；另对固定上游和审计提交的逐文件 Git blob SHA-1/工作树 SHA-256 清单作比较。具体哈希、退出码与日志见 `PIR_PSI_NON_INVASIVE_ACCEPTANCE.md`。

原作者 Nexus-AM 固定提交 `631f997` 是参考，不是要求逐字恢复。对比该提交与审计提交 `13c1c458`，原作者**已有文件**只有四个被修改：`tests/hputest/Makefile.native.case`（接入可选目标案例）、`tests/hputest/scripts/import_package.py`（应用包严格导入/校验）、`tests/hputest/scripts/build_performance.py`（目标静态构建及事件验证）、`tests/hputest/scripts/test_build_performance.py`（对应拒绝测试）。其余 25 个路径均为 `tests/hputest` 下新增的 PIR/PSI 应用、运行时、脚本、测试和文档。本轮 Nexus-AM 新分支从 `13c1c458` 创建，暂不再改上述四个已有文件；保留先前审计的 CPU、内存、映射和构建验证修改。
