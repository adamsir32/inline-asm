# PIR/PSI 审查修复逐文件清单

基线：inline-asm `5a5392ffa15ba9648a623db60072b54589f1eb34`；Nexus-AM `5f51a4a364612c028961b1aa5033f60b57c16135`。

| 仓库 | 状态 | 路径 | 改动目的 |
|---|---|---|---|
| `adamsir32/inline-asm` | M | `CMakeLists.txt` | 增加 PSI 两根碰撞场景的 CTest 入口。 |
| `adamsir32/inline-asm` | M | `doc/delivery/PIR_PSI_SMALL_ACCEPTANCE_REPORT.md` | 澄清 Small 配置中实际每桶上限 16 与历史描述的差异。 |
| `adamsir32/inline-asm` | M | `examples/bfv_apsi_concentrated_application.cpp` | 修正桶容量边界，构造同桶两根碰撞/未命中，并添加客户端从目标 DDR 重建密文的入口。 |
| `adamsir32/inline-asm` | M | `examples/bfv_sealpir_2d_application.cpp` | 以唯一哨兵编译 Stage 1，再物理替换 96 个明文 limb、执行固定指令并同原生 SEAL 逐系数对照；写入实际数据的发布包。 |
| `adamsir32/inline-asm` | M | `examples/pir_physical_binding.hpp` | 按逻辑明文身份与模数绑定物理分配，检查地址、长度和编码内容。 |
| `adamsir32/inline-asm` | A | `examples/psi_client_readback.hpp` | 从目标 C 导出的 DDR 按输出元数据/位序重建 PSI 密文，交由客户端 SecretKey 解密。 |
| `adamsir32/inline-asm` | M | `test/apsi_concentrated_delivery_test.py` | 覆盖 PSI 场景及实际执行镜像导出。 |
| `adamsir32/inline-asm` | M | `test/pir_dynamic_stage_handoff_test.py` | 验证哨兵唯一性、重复内容替换、边界索引、两组数据库和损坏/错误映射拒绝。 |
| `adamsir32/inline-asm` | M | `test/pir_physical_binding_test.cpp` | 覆盖逻辑绑定成功与歧义、缺失、内容不符等拒绝。 |
| `adamsir32/nexus-am` | M | `tests/hputest/Makefile.native.case` | 把统一 PIR 目标的压缩镜像和解压运行时纳入构建。 |
| `adamsir32/nexus-am` | M | `tests/hputest/docs/PIR_PSI_SMALL_ACCEPTANCE.md` | 记录 Small 配置、碰撞与客户端密文交接的验收边界。 |
| `adamsir32/nexus-am` | M | `tests/hputest/docs/PIR_UNIFIED_CPU_HPU_RUNTIME.md` | 说明 PZL1 镜像布局、加载校验与统一程序边界。 |
| `adamsir32/nexus-am` | M | `tests/hputest/runtime/pir/fixture.S` | 以压缩后的镜像字节装入统一目标 ELF。 |
| `adamsir32/nexus-am` | M | `tests/hputest/runtime/pir/pir_unified_runtime.c` | 实现 PZL1 解码、长度/边界校验及目标 DDR 预置。 |
| `adamsir32/nexus-am` | M | `tests/hputest/runtime/pir/pir_unified_runtime.h` | 声明统一目标镜像准备与校验接口。 |
| `adamsir32/nexus-am` | M | `tests/hputest/scripts/build-pir-psi.py` | 生成与校验压缩目标镜像，保留包来源和大小检查。 |
| `adamsir32/nexus-am` | M | `tests/hputest/scripts/pir_unified_layout.py` | 定义并生成无损 PZL1 行压缩格式。 |
| `adamsir32/nexus-am` | M | `tests/hputest/scripts/stage1_patch.py` | 验证 32×3 哨兵身份、位置、内容哈希和物理输入覆盖。 |
| `adamsir32/nexus-am` | M | `tests/hputest/scripts/test_pir_unified_control.py` | 增加统一主程序的控制/故障覆盖。 |
| `adamsir32/nexus-am` | M | `tests/hputest/scripts/test_pir_unified_memory_guard.py` | 增加压缩流损坏和内存越界拒绝测试。 |
| `adamsir32/nexus-am` | A | `tests/hputest/scripts/test_psi_ciphertext_handoff.py` | 运行模型输出到目标 C DDR 再到客户端 SEAL 解密的完整交接及损坏负例。 |
| `adamsir32/nexus-am` | M | `tests/hputest/scripts/test_psi_single_main_integration.py` | 从实际模型 DDR 导出目标 C 密文输出并验证只读区与完成状态。 |
| `adamsir32/nexus-am` | M | `tests/hputest/src/08_pir_psi/08_04_pir_unified.c` | 在统一入口检查镜像解压、阶段交接及失败路径。 |

| `adamsir32/inline-asm` | A | `doc/delivery/PIR_PSI_AUDIT_FIXES_REPORT.md` | 汇总修复、软件验收、构件大小、限制与证据路径。 |
| `adamsir32/inline-asm` | A | `doc/delivery/PIR_PSI_AUDIT_FIXES_FILE_LIST.md` | 逐文件说明本轮全部代码和交付文档变更。 |
| `adamsir32/inline-asm` | A | `doc/delivery/PIR_PSI_AUDIT_FIXES_TEST_MATRIX.md` | 分层记录测试命令、预期与实际退出码。 |
| `adamsir32/inline-asm` | A | `doc/delivery/PIR_PSI_AUDIT_FIXES_SHA256.txt` | 保存 1,638 个正式验收构件与日志的完整 SHA-256 清单。 |
| `adamsir32/inline-asm` | A | `doc/delivery/PIR_PSI_AUDIT_FIXES_EXTRA_SHA256.txt` | 保存 15 个补充边界/碰撞/交接证据的 SHA-256 清单。 |

生产者 `include/`、`src/`、`encode/` 与基线逐字相同；Nexus-AM 改动仅在 `tests/hputest/`。最终报告和 SHA 清单为验收后的文档新增文件。
