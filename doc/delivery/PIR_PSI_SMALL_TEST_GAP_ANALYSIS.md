# PIR/PSI Small 完整测试缺口分析

固定基线：inline-asm `8c66404bfa765cbea94339fbfd720f718572904b`；Nexus-AM `9cd7a544bcc17cddcc374e3fb7e2f18f3ae167fd`。两条个人仓库远端分支与指定 SHA 一致，两个本地工作树在审查开始时干净。本轮从这两个提交分别创建 `feature/pir-psi-small-acceptance`。原作者 `include/`、`src/`、`encode/` 为禁止修改区域。

## 调用链

PIR：确定性数据库与查询 → SEAL 参考密文 → `ApplicationBuilder` 第一阶段图和包 → 原有编码指令执行器及逐系数 Golden 校验 → 完整执行后镜像 → CPU 分解 32 明文 → 第二阶段 96 个编码分量与公开物理分配唯一匹配 → 固定第二阶段指令对原始/变化数据库分别重放 → 解密及 1024 字节记录对照。Nexus-AM `08_04` 在同一个 CPU `main()` 中顺序发射两阶段，读取实际 DDR 并运行 CPU 分解、编码及地址/只读保护。

PSI：Sender/Receiver 集合 → 确定性 OPRF 输出夹具 → Trident Kuku 哈希分桶 → SenderDB 根多项式 → PowersDag 和 Paterson–Stockmeyer → SEAL 查询幂次与应用图 → 编译包、逐节点 SEAL oracle、功能 HPU 模型及编码指令模型 → 解密五字段全零槽位 → 与 Trident 概率性 CPU 参考交集比较。Nexus-AM `08_01` 的目标 `main()` 负责镜像准备、HPU 发射/中断、输出与只读区逐字检查；客户端解密和交集恢复目前位于生产者应用侧。

## 逐文件盘点

| 文件 | 用途和现有功能 | 已有测试 | 缺口及本轮处理 |
|---|---|---|---|
| `examples/bfv_sealpir_2d_application.cpp` | N=4096 Small 数据/查询、两阶段图、SEAL 对拍、编码指令执行、完整内存、CPU 分解、动态重放、最终记录 | Small 记录、两输入 handoff、22 项相关 CTest | 功能齐备；不改算法，复用干净构建和新输入运行 |
| `examples/pir_physical_binding.hpp` | 以完整编码内容、模数、长度和分配属性唯一匹配 96 个物理分量；歧义拒绝 | `pir_physical_binding_test.cpp`、handoff | 无功能缺口；纳入统一负例记录 |
| `test/pir_dynamic_stage_handoff_test.py` | 从新包运行两阶段、检查非输出内存变化、两组数据库、独立记录 oracle 和损坏输入 | 已在前轮服务器 PASS | 直接复用；统一入口需保证新输出目录及退出码 |
| `test/sealpir_2d_delivery_test.py` | 小规模包/记录/manifest 校验 | CTest | 可复用；动态 handoff 覆盖更完整主路径 |
| `test/apsi_concentrated_delivery_test.py` | Small/Medium/Target 包和数值标志校验 | Small empty/single/multi/boundary CTest | 已有四场景；统一入口应强制本次新包、输入、参考交集与模型状态一致 |
| `examples/bfv_apsi_concentrated_application.cpp` | N=8192 Small 集合、OPRF 夹具、Kuku、SenderDB、PowersDag/PS、SEAL/HPU 对拍、解密 | 四场景、SenderDB 13 组、完整 HPU 2 组 | 缺少给目标 C 宿主联动使用的**真实编码指令执行后镜像**导出；新增应用级可选导出，不碰核心 |
| `test/apsi_trident_duplicate_input_test.py` | 重复元素去重及非法/空集合拒绝 | CTest | 已有，统一异常清单复用 |
| `test/apsi_trident_cross_felt_test.py`、`test/apsi_trident_senderdb_differential.py` | 跨字段、边界和原生 SenderDB 差分 | 13/13、HPU 2/2 的前轮记录 | Small 闭环复用相关场景；不扩大到正式规模 |
| `tools/verified_pir_psi_delivery.py` | 干净提交、新构建目录、来源和包校验 | 来源负例及前轮正式交付 | 当前 PSI 模式固定 multi，且分别配置构建；统一入口可在一个新构建目录调用现有例程并执行四场景 |
| `tests/hputest/scripts/build-pir-psi.py` | 包导入、DMA/来源校验、四目标 ELF/BIN 构建及静态检查 | 前轮四目标构建 | 复用；统一入口须把本次包与准确提交传入 |
| `tests/hputest/src/08_pir_psi/08_04_pir_unified.c`、`runtime/pir/*` | 同一 CPU 主程序的两阶段 DDR、分解编码、固定指令和故障处理 | CPU 181 单测、11 主控、宿主联动 | 完整复用并从本次新包重跑 |
| `tests/hputest/src/08_pir_psi/08_01_psi_single.c`、`application_case.inc` | 单包镜像准备、HPU 发射/中断、Golden 和只读区校验 | ELF/BIN 静态检查 | **缺目标 C 软件联动执行**；增独立宿主桩和真实模型 DDR，测正常及输出/只读区损坏拒绝 |
| `tests/hputest/runtime/native/image.c`、`check.c` | 目标镜像准备、逐字 Golden、只读区检查 | 现有静态与单元测试 | 在 PSI 宿主联动中编译运行原文件，避免重新实现检查逻辑 |
| `tests/hputest/scripts/test-pir-psi-negative.py` | 包缺文件、ABI、指令、DMA 越界、Golden、镜像和动态格式错误 | 前轮独立执行 | 统一入口复用，并记录每项预期拒绝 |
| `tests/hputest/scripts/test_pir_unified_main_integration.py` | 真 CPU 代码 + 模型 DDR 的 PIR 目标 C 宿主联动 | 前轮正常/两故障 PASS | 从本次新交付重跑 |

## 验证等级与待解决项

现有应用算法、SEAL 数值对拍、HPU 功能模型、编码指令模型和 Nexus ELF/BIN 构建均已有实现。本轮补足 PSI 目标 CPU 宿主联动及跨仓库统一复验；报告逐级列出实际退出码。Nexus `08_01` 不在目标 CPU 上执行 SEAL 解密，交集恢复由生产者客户端完成，须在报告中分别验证这两个边界。真实 HPU/RTL 为 **NOT RUN**。
