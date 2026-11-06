# PIR/PSI 独立审查问题修复与软件验收报告

本报告仅将所附独立审查 PDF 作为问题线索；结论以服务器源码、实际执行、包校验和构件为依据。所有 C++/Python 测试及 RISC-V 构建直接在 SSH 服务器 `zhaozhengyi@10.102.104.22:9322` 完成。

## 固定版本与结论

- 生产者基线：`5a5392ffa15ba9648a623db60072b54589f1eb34`；本轮干净验收代码：`2622761537c7dcd73a89da3482da1d65c2b9c9b5`。
- Nexus-AM 基线：`5f51a4a364612c028961b1aa5033f60b57c16135`；本轮干净验收代码：`c738b84e1fb7a267c3042bad4144d1ae794b54f3`。
- 两仓库分支均为 `feature/pir-psi-audit-fixes`。生产者 `include/`、`src/`、`encode/` 零差异；Nexus-AM 改动仅在 `tests/hputest/`。
- 最终验收目录：`/home/zhaozhengyi/independent-pir-psi-audit-20261026/audit-fixes-clean-final4`；`summary.json` 状态 **PASS**、21 个记录步骤符合预期；7 个正式应用包通过校验。先前探索性失败目录不作为通过证据。
- 目标级仿真、RTL 与实体 HPU：**NOT RUN**。目标 C 的宿主联动由 Mock HPU 接入软件执行器生成的 DDR 镜像，不等于硬件算术执行。

## 问题处理

### F-01：PIR 动态物理位置绑定

- Small PIR 为 N=4096、128 条记录、每条 1024 字节；每个明文最多容纳 9 条记录，实际使用 16 个明文组成 4×4 物理矩阵。Stage 0/1 分别执行 17,740/26,700 条 HPU 编码指令。

- Stage 1 先编译 32 个独特的非零哨兵明文，使三个 Q 模数下每个逻辑输入具有独立物理分配；编译器对相同明文会合并分配，因此不能仅按明文内容匹配。
- 从哨兵模板的公开分配表确认 32×3=96 个 allocation ID、地址、长度、编码内容与 FNV1a 校验。客户端把实际 CPU 分解/编码的 96 个 limb 写到这些固定地址，固定指令流不重新编译。
- 物化发布包的初始镜像为实际输入，Golden 为固定编码指令执行器的实际输出，并逐对象、分量、模数和系数同独立原生 SEAL Stage 1 对照。编译器完整报告保留，另附动态物化证明字段。临时哨兵编译包不作为正式发布包。
- 同内容的 32 个替换明文仍占 32 个不同物理位置；缺失/重复/篡改哨兵、缺失/非规范动态 limb 和交换 limb 的负例均拒绝。索引 0、8、9、127 以及两组数据库的 1024 字节记录均与独立记录真值一致。

### PSI 同桶两根与密文交接

- Small 测试通过实际 Kuku hash 找到同桶两个发送者元素（示例 60、89，桶 1755），构造二次根多项式；first、second、miss 三个场景 3/3 PASS，容量边界使用正确的含等号判定。
- 现有四个 Small 场景 empty/single/multi/boundary 全部 PASS。实际 Small 配置为 Sender 10、Receiver 5、N=8192、Kuku 4096 桶、单窗口 1024 桶、每桶上限 16；填充项不算有效集合元素。OPRF 为确定性测试输入，不包含网络 FourQ 流程。
- `08_01_psi_single` 目标 C 从 Mock HPU 所见的真实模型 DDR 输出导出密文；客户端按输出元数据恢复 SEAL Ciphertext、与独立 SEAL 参考逐字比对、用客户端 SecretKey 解密并恢复交集。最终干净包测试 host=0、client=0、corrupt=1。Golden 只用于对照与内存保护，没有代替目标 C 输出。

| PSI Small 场景 | Sender 有效元素 | Receiver 有效元素 | 预期交集个数 |
|---|---:|---:|---:|
| empty | 10 | 5 | 0 |
| single | 10 | 5 | 1 |
| multi | 10 | 5 | 2 |
| boundary | 10 | 5 | 2 |

### F-03：统一 PIR ELF 体积

- 核查现有 Nexus-AM ELF/BIN 加载与 DDR 机制后，采用无损 PZL1 行压缩、目标端解压和长度/边界检查；目标镜像分阶段存放。没有假设不存在的外部 DDR 镜像加载器。
- 两阶段编码指令流与原验收包逐字一致：Stage 0 SHA-256 `bd7672999e82a37e2254942dd61ba7c8b2ed0cdf4f51b45a8c85779f814d97c7`，Stage 1 SHA-256 `d541de297369100e44c725468753d6148454b6e74cfa4979e582882056fe60e1`；分别为 17,740 与 26,700 条指令。
- Stage 0 原始镜像 30,701 行 / 7,859,456 字节；Stage 1 为 45,549 行 / 11,660,544 字节。导入目标加 64 guard 行后分别为 7,875,840 与 11,676,928 字节；PZL1 文件分别为 2,649,532 与 3,698,236 字节。解压损坏、越界和内存守卫负例均拒绝。
- 本修复使统一 ELF/BIN 显著缩小，但压缩流及 Golden 仍在 ELF 内，阶段切换仍解压/复制至 DDR。外部镜像加载和仅切换 CSR 基地址尚未实现；这部分 F-03 目标应标为**未完成**，上板前须结合真实加载器再处理。

| 统一目标 | 修复前 | 修复后 | 变化 |
|---|---:|---:|---:|
| ELF | 21,526,632 B | 8,322,832 B | 减少 61.34% |
| BIN | 23,159,856 B | 9,955,888 B | 减少 57.01% |
| `.rodata` | `0x1385258` | `0x6ed658` | 缩小 |

## 四个 RISC-V 目标构件

| 目标 | ELF 字节 / SHA-256 | BIN 字节 / SHA-256 |
|---|---|---|
| `08_01_psi_single` | 6299880 / `4159b347d773f99b6282ce4b152e099f88a3fe453097213ed838744f4db8be91` | 6227256 / `2eb7cdabad9d3c227e83da3393f30a216457caea07847fc53721f1d8f45a799d` |
| `08_02_pir_stage0` | 8609152 / `e82da5bd3eaeb24dbc7d6e5f0fb095112f72c60695c4a66086da2f5dfeb164b0` | 8283400 / `f7768d157323f126209c53ca3f1f4dbf612a02e148b96e179889489f7f6a94a0` |
| `08_03_pir_stage1` | 12891840 / `81c863a593f6975f4338f01dd26f7b8e8fac134b1b2a21d1be58169fd1c6ae22` | 12412104 / `ebf227a7e878e0cae6d0774eb5cd010c400b350e6da1ec9cf6f2e5bb85251eb0` |
| `08_04_pir_unified` | 8322832 / `6d7365284c8148d36c3f912a04b0ad503b0c5c7291d2a3712334944c2bd590f6` | 9955888 / `df6d90f804271ef907eeda5a624dc0f703b201e362b768ae607e92ea4a5ae5a4` |

四个 receipt 均为 `TARGET_BUILD_AND_STATIC_CHECK_PASS`，包含导入包来源、DMA/内存布局及 ELF 段边界检查。统一目标的实际目标 C 主程序在宿主桩环境执行正常路径和故障路径；这不是 RTL/实机测试。

## 回归与证据

- 干净 Release 构建与 22/22 生产者 CTest PASS；PIR 索引 8 完整动态交接、四个 PSI Small 场景、四目标构建、PIR CPU 编码重放、PIR/PSI 目标 C 宿主联动和包/动态输入负例均由 `run-pir-psi-small-acceptance.py` 一次完成。21 步的精确命令、工作目录、退出码、耗时和日志路径在 `summary.json`。
- 另补索引 0、9、127 的完整交接均退出 0；Kuku 两根碰撞 CTest 3/3 PASS；Nexus Python 181 项 PASS（2 skipped）；最终干净包的 PSI C→客户端密文交接 PASS；Stage 1 独立包导入与 RISC-V 编译 PASS。
- 主验收目录的 1,638 个非编译中间构件和日志 SHA-256 见 `PIR_PSI_AUDIT_FIXES_SHA256.txt`，在目录内运行 `sha256sum -c checksums.sha256` 为 1,638/1,638、退出码 0。目录外 15 个补充证据见 `PIR_PSI_AUDIT_FIXES_EXTRA_SHA256.txt`，`sha256sum -c` 亦退出 0。
- `summary.json` SHA-256：`28162b5285c56c6f6c3e57f9af507f2efccc7e04cb42cbb9bc83b619d7082614`；主清单 SHA-256：`a70e5d80033e7884641d29c4a4766beefdacd2694e5ad948f66a2bf7383eb7ca`；额外清单 SHA-256：`8cd79c4300d863ab4c9767effaec2e211577c253dc2ce64c0c0414d8d4a469e1`。
- 逐文件理由见 `PIR_PSI_AUDIT_FIXES_FILE_LIST.md`，分层测试结果见 `PIR_PSI_AUDIT_FIXES_TEST_MATRIX.md`。

## 保留限制

- F-04：Nexus-AM PSI 目标端只覆盖单个 1024 桶窗口；Receiver 跨窗口需要多包连续调度测试，本轮未实现。
- F-05：PIR CPU C 运行时固定 N=4096、20 位明文、reverse_12 和 16/96 limb；扩展规模前需参数化。
- 正式 PSI 17 分区、多窗口连续调度、通用 PIR 运行时、真实 FourQ 网络 OPRF、性能压力测试、目标级仿真、RTL 和实体 HPU 均未在本轮执行。
- 部分历史 PDF/报告中的统一 ELF >30 MB、PSI 每桶上限 2 等数字对应旧快照或不准确描述；本报告只给出此次固定基线和实际源码/构件测量。
