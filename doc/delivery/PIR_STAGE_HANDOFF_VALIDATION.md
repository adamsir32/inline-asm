# PIR 两阶段交接验证边界

> 后续进展（独立分支）：真实编码指令模型的执行后内存导出、CPU 逐系数读回与分解、第二阶段固定指令动态重放已完成软件数值验证。见 [动态交接报告](PIR_DYNAMIC_STAGE_HANDOFF_REPORT.md)。下文保留此前基线的状态和分析，文中的 BLOCKED 指当时状态。

## 现有路径与真实包格式

`concentrated_stage_hpu` 对每个导出结果调用 `verified_oracle_result`；该函数先要求软件模型与 SEAL 逐系数相等、主机指令模型验证通过，随后返回 **SEAL oracle 密文**。默认 `decompose_to_plaintexts` 因此处理的是已核对的参考代理，而非从最终 HPU_MEM 取出的数据。`CompiledApplication` 公开 `package()` 和 `oracle_result()`，没有执行后完整内存的读回 API。`package().outputs[].golden_words` 来自 SEAL 参考，编译阶段逐 word 与内部 `BfvSoftwareExecutor::memory().words()` 核过，仍不能把 Golden 称为独立读回。

既有正式 PIR 包 `stage0_concentrated_000`：N=8192，输出 31 个 `reply_i`，每个 2 个密文组件 × 2 个 Q limb，Golden 共 124 个 limb；阶段 0 模数链从 Q3 切到 Q2，Q2=`[2146959361,2147074049]`，`chain_index=1`。`golden/golden_manifest.csv` 为每个 limb 给出 `object_id/component/modulus_id/modulus/domain/path/line_offset/line_count`，域均为 `coefficient_bit_reversed`。物理 word `physical[j]=natural[BR_8192(j)]`，每 limb 独立反转；不得把整个密文或 RNS 顺序一起反转。CPU 密文内存要求组件优先、每组件 limb 优先、limb 内自然系数顺序，非 NTT。第二阶段包同为 N8192、Q2 输出，8 个结果、32 个 Golden limb。两个包使用 `hpu-objects64-v1.2`、32-bit 小端 word、每 line 64 word/256 B。

`decompose_to_plaintexts` 对每个 Q2 limb 的 31 位系数切为两个 20 位明文块，顺序为 reply → component → modulus → chunk → coefficient。目标 31 个 reply × 2 组件 × 2 limb × 2 chunk = 248 个明文；`compose_to_ciphertext` 用相反顺序恢复，`decode_recursive_reply` 再解密并恢复 288 B 记录。第二阶段把这 248 个明文通过 `ApplicationBuilder::import_plaintext` 编进新的初始镜像；当前每次更换输入都会重新创建图、编译、生成包。尚无已验证的“只替换镜像、复用同一段指令”的目标侧 ABI。

## 新增外部读回入口

`--stage0-readback PATH` 要求 PATH 为**本次第一阶段执行后的完整 HPU_MEM 镜像**，长度严格等于该次编译包的 `used_lines×256`。应用逐项从输出 span 读取小端 word，逐字比对该次包的 Golden，检查组件、Q 模数、长度、重复和缺失，然后对每个 limb 做 BR→自然系数转换；密文数据全部由文件覆盖。成功时打印 `AUDIT_STAGE0_READBACK_FNV1A64`、`AUDIT_STAGE1_INPUT_FNV1A64`、`AUDIT_RECORD_FNV1A64`，随后运行现有 CPU 分解和第二阶段软件计算，并与独立记录生成函数逐字节比较。FNV 用于快速定位不同运行；交付归档仍须对完整镜像和包计算 SHA-256。生成该镜像的设备或软件执行器必须与本次查询密钥、指令和包对应；不同运行的随机密文不能混用。

合成适配器测试先用第一阶段包的初始镜像和 Golden 构造测试镜像，再执行上述入口；损坏一个输出字节或截短文件均被拒绝。它验证了读取、元数据定位、位序转换、CPU 分解和阶段二输入数值路径，**没有证明阶段一执行后内存可以被独立导出**。目前无独立软件执行结果文件，所以 B2 理想链的第 2 步仍是 **BLOCKED**；不能把合成镜像或参考对象复制说成独立读回。要封闭这一缺口，原作者需在 `CompiledApplication` 或软件执行器公开一个只读、带本次编译身份和 `used_lines` 的最终内存导出接口；应用层无法从当前 `package()` 逆推出实际执行后字节。该接口应由 HPU 编译器/运行时维护者设计和审查，本分支未动核心。

| 模式 | 阶段一来源 | 阶段二输入 | 验证状态 |
|---|---|---|---|
| 固定输入软件回归 | 已核对的 SEAL oracle 代理 | 当前 CPU 分解后重新编译包 | 小规模 PASS；正式规模 HISTORICAL |
| 外部读回适配 | 外部完整 HPU_MEM 镜像，逐字对包 Golden | 文件数据经 BR 转换、CPU 分解后重新编译包 | 合成适配器 PASS；独立软件/硬件读回 BLOCKED |
| 目标侧运行时替换 | 未来 HPU DSTORE/DDR 实际读回 | 不重编译而重装第二阶段镜像 | NOT RUN，ABI 未确认 |

本轮小规模索引 0、8、9、64、127 覆盖首条、中间、末条及相邻明文边界的 8/9；旧正式 Trident 65535 记录结果仅作历史证据。目标侧新读回仍需重复这些边界。证据命令和日志见[软件收尾报告](PIR_PSI_SOFTWARE_HARDENING_REPORT.md)。
