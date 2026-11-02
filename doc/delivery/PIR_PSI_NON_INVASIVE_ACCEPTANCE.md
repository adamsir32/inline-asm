# PIR/PSI 非侵入式重构差分验收

受保护上游：inline-asm `cfee951e4056660e521d55a3535d82335a49bb08`。重构前：inline-asm `bc2c40d535413a30a7dfc4bb3323aecda3ca18e2`、Nexus-AM `13c1c4583fbaf5d10f975332d77db87596faaf7e`。重构代码提交先为 `2063dc154180931f852e8b569d60c9387cc0ac6f`，交付脚本修复后为 `b78e1395a5a6da8d429492de1e3c9a5a5230367d`；最终报告/分支提交以本次结束时 `git rev-parse HEAD` 为准。全部实际构建与测试在服务器 `zhaozhengyi@10.102.104.22:9322`，证据根目录 `$A=/home/zhaozhengyi/independent-pir-psi-audit-20261026`。

## 关键对照

| 项目 | 命令或证据 | 状态 |
|---|---|---|
| 侵入改动清单 | `git diff --name-status cfee951e bc2c40d -- include/ src/` | PASS：恰 8 个已修改文件，无新增文件 |
| 固定上游 Git 树与逐文件 SHA-256 | `git diff --exit-code cfee951e HEAD -- include/ src/`，`git rev-parse <rev>:include/src`；脚本逐文件比较 `git show cfee:<path>` 与工作树并列举所有额外文件 | PASS：`include` 树 `4a22d29badc7956d6dab442d67768aedfd06c6bd`，`src` 树 `b5056b999ea0464118538101dd6716c3df5778bc`；153/153 文件逐字相同、额外 0、缺失 0，`$A/non-invasive-evidence/include-src-sha256.txt` |
| 应用层替代方案先行验证 | `$A/non-invasive-evidence/pre-restore-handoff.log`；原审计核心尚在，应用改走公开指令执行器/内容匹配 | PASS：双输入重放 |
| 核心恢复后的开发构建 | `$A/non-invasive-evidence/post-restore-configure.log`、`post-restore-build.log` | PASS：独立新构建目录 |
| 核心恢复后的相关测试 | `ctest --test-dir $A/non-invasive-build-post-restore-dev --output-on-failure -R '<19项原回归+包测试+绑定/来源测试>'`；完整表达式在命令日志 | PASS：22/22；`post-restore-ctest.log` |
| 最终干净提交相关测试 | `cmake --build $A/non-invasive-verified-build-b78 -j4` 后对同一 22 项执行 `ctest --test-dir $A/non-invasive-verified-build-b78 --output-on-failure -R '<同上表达式>'` | PASS：构建退出码 0，22/22、0 failed；`clean-b78-all-build.log`、`clean-b78-ctest.log` |
| 双输入动态阶段交接 | `python3 test/pir_dynamic_stage_handoff_test.py --example ... --stage1-patch-script ... --output $A/non-invasive-evidence/post-restore-dev-handoff` | PASS；`post-restore-dev-handoff.log` |
| 旧审计包与开发构建逐字比较 | `$A/non-invasive-evidence/post-restore-dev-comparison.txt` | PASS：13/13 文件逐字相同 |
| 受控脚本首次发布负例 | `$A/non-invasive-evidence/verified-pir-2063.log` | 正确拒绝：测试生成的 Python `__pycache__` 令源码树变脏；未发布输出。随后修复并从新提交重跑 |
| 受控脚本从干净提交发布 PIR | `python3 tools/verified_pir_psi_delivery.py --source-root $A/non-invasive-producer --build-dir $A/non-invasive-verified-build-b78 --output $A/non-invasive-verified-pir-b78 --expected-commit b78e1395a5a6da8d429492de1e3c9a5a5230367d --mode pir-dynamic --stage1-patch-script $A/non-invasive-nexus/tests/hputest/scripts/stage1_patch.py --prefix $A/trident-faithful-reference/reference-prefix --cmake-arg=-DFETCHCONTENT_SOURCE_DIR_MSGSL=... --cmake-arg=-DFETCHCONTENT_SOURCE_DIR_ZLIB=... --cmake-arg=-DFETCHCONTENT_SOURCE_DIR_ZSTD=...` | PASS；`verified-pir-b78.log`，收据 `non-invasive-verified-pir-b78/verified_delivery_receipt.json` |
| 干净交付计算数据逐字对照 | `$A/non-invasive-evidence/verified-pir-comparison.txt` | PASS：13/13；包含两阶段机器字、DMA、初始镜像、stage0 完整最终内存、CPU 明文、两组动态镜像及记录 |
| 受控脚本从干净提交发布 PSI 小包 | `python3 tools/verified_pir_psi_delivery.py --source-root $A/non-invasive-producer --build-dir $A/non-invasive-verified-psi-build-b78 --output $A/non-invasive-verified-psi-b78 --expected-commit b78e1395a5a6da8d429492de1e3c9a5a5230367d --mode psi-small --prefix ... --cmake-arg=...` | PASS；`verified-psi-b78.log`，SEAL oracle、HPU 功能/编码指令软件模型和包校验均通过；RTL NOT RUN |
| PSI 与旧审计包数据对照 | 比较 `set_test_data.json`、小包初始镜像、DMA 清单、编码指令；`$A/non-invasive-evidence/psi-comparison.txt` | PASS：4/4 逐字相同，数学输入与指令不变 |
| Trident SenderDB 原生差分 | `python3 test/apsi_trident_senderdb_differential.py --native $A/trident-faithful-reference/trident-apsi-build/apsi/bin/apsi_native_senderdb --params $A/trident-faithful-reference/trident/apsi/parameters/1M-1024-com.json --example $A/non-invasive-verified-psi-build-b78/hpu_bfv_apsi_concentrated_example --case-dir $A/trident-faithful-reference/senderdb-cases --output $A/non-invasive-evidence/senderdb-differential-b78 --full cross_felt extra_bundle_126` | PASS：13/13，完整 HPU 软件求值 2/2；`senderdb-differential-b78/summary.json` |
| PSI Medium 计算 | `/usr/bin/time -v timeout 1200 $A/non-invasive-verified-psi-build-b78/hpu_bfv_apsi_concentrated_example --profile medium --scenario multi` | PASS，退出码 0；N=4096、t=188417、五字段、2 bundles、1 sender partition，SEAL oracle/HPU 功能/编码指令软件模型和最终匹配均 PASS；`psi-medium-b78.log`，RTL NOT RUN |
| 目标 C 分解/编码与两输入执行 | `python3 $A/non-invasive-nexus/tests/hputest/scripts/test_pir_cpu_algorithms.py --fixture $A/non-invasive-verified-pir-b78 --output $A/non-invasive-evidence/cpu-clean-b78 --dynamic-input-map .../dynamic_inputs.tsv --replay-executable $A/non-invasive-verified-build-b78/pir_cpu_encoded_replay` | PASS；`cpu-clean-b78.log`、`summary.json`：`32×4096`、`96×4096`，双输入 26,700 指令/9,642 DMA |
| 受控脚本来源负例 | `test/verified_pir_psi_delivery_guard_test.py` | PASS：错误提交、脏树、旧构建目录、错误包来源均拒绝 |
| 旧构建目录实际拒绝 | `python3 tools/verified_pir_psi_delivery.py --source-root $A/non-invasive-producer --build-dir $A/non-invasive-verified-build-b78 --output $A/non-invasive-evidence/should-not-exist --expected-commit b78e139... --mode pir-small` | PASS（预期退出码 1），`old-build-negative.log` 明确拒绝旧构建目录，未生成输出 |
| Nexus-AM Python 单测 | `python3 -m unittest discover -s $A/non-invasive-nexus/tests/hputest/scripts -p 'test_*.py'` | PASS：181 项、2 skipped；`$A/non-invasive-evidence/nexus-unittests.log` |
| 统一主控故障测试 | `python3 $A/non-invasive-nexus/tests/hputest/scripts/test_pir_unified_control.py` | PASS：11 场景；`unified-control.log`，宿主机桩函数而非 RTL |
| 历史正式 PSI 分区包格式 | 用本次恢复后的 `hpu_validate_package` 遍历 `$A/trident-alignment-20261026/exact-target-latest-delivery/hpu_query_partition_*` | PASS：17/17，`psi-17-package-validate.log`；这些包是旧提交 `e15a66624aef81c6857086da82e1b2e93b56fe51` 生成，本次没有重新生成正式规模包 |
| Nexus-AM 四个目标 | `python3 tests/hputest/scripts/build-pir-psi.py --case <case> --producer-commit b78e139... --stage0-package ... --stage1-package ... --cpu-parameters ... --dynamic-input-map ... --package-validator ... --output $A/non-invasive-targets-b78 --am-home $A/non-invasive-nexus --cross /opt/riscv/bin/riscv64-unknown-elf-` | PASS：`08_01_psi_single`、`08_02_pir_stage0`、`08_03_pir_stage1`、`08_04_pir_unified` 的 ELF/BIN 与静态检查均成功；各自 `08_0*-build.log`，完整哈希见下文 |
| 统一主程序目标 C 宿主联动 | `python3 $A/non-invasive-nexus/tests/hputest/scripts/test_pir_unified_main_integration.py --delivery $A/non-invasive-targets-b78/imported/08_04_pir_unified --fixture $A/non-invasive-verified-pir-b78` | PASS：同一 CPU `main()` 正常两阶段计算、动态输入写入损坏和输出损坏两类故障均被拒绝；`main-host-b78.log`；目标级仿真仍 NOT RUN |
| Nexus-AM 默认构建 | `python3 $A/non-invasive-nexus/tests/hputest/scripts/build-runtime.py --am-home $A/non-invasive-nexus --arch riscv64-xs --cross /opt/riscv/bin/riscv64-unknown-elf- --producer $A/baseline/original-producer --output $A/non-invasive-default-109 --jobs 4` | PASS：109/109 ELF/BIN 构建及静态/周期边界检查；`default-109.log`，ZIP 哈希见下文；RTL NOT RUN |

## 目标产物 SHA-256

| 目标 | ELF SHA-256 | BIN SHA-256 |
|---|---|---|
| `08_01_psi_single` | `bf38264d0141fc2c3bf19280c9ff0c55b7efdb90359b261ce667fec6912312ff` | `60aa4a2ca363c4615bf469673017dbb2d18b53cfc00d7baf96dd20bd43256f1e` |
| `08_02_pir_stage0` | `688dc3c5cbb9ebeb846076b0fbdfcf09a06c8bf5789df89026e6eb369732029b` | `7a293179665f7b5781c0e77c3f7190654ea739d9313c2ea030510ff6e6a6318c` |
| `08_03_pir_stage1` | `6ab50957eab89b9c04b5b101c036370d4af57437c33b4f18c1c963bd4bf4b962` | `8e2b49fbdcc9a536c52b13daf3fd12f9dda3c6b31af47a62b477ca29dc75620c` |
| `08_04_pir_unified` | `11e7101d2f2fed4a2ef3dd58ee3d019e69ae4026870607d9db164911ba31637a` | `e47672ba63471f139c51345112d9d3e981588e75837af6bcb4146c05c86f9aee` |

默认 109 项 ZIP：`f7f409d128e3b605b3751ea3bb9cd918aee3c6964f5f52b06a38787c166a2dfe`，66,969,482 字节。

## 失败、边界及未运行项目

首次在 `2063dc1` 上运行受控交付时，Python 测试在源码目录生成未跟踪 `__pycache__`，脚本按设计拒绝发布（PASS，预期拒绝）。在 `b78e139` 中给子进程设置 `PYTHONDONTWRITEBYTECODE=1` 后，以新的干净构建和输出目录重跑并成功发布。最终 CTest 的直接运行也会生成未跟踪的 Python 缓存；测试完成后仅清理 `test/__pycache__`、`tools/__pycache__`，重新核对源码工作树清洁。没有改变被测程序或测试判定。

动态明文若出现同模数、同编码内容的两个物理候选，应用映射明确拒绝，不能由现有公开包数据唯一绑定。该情况的正确拒绝测试 PASS；支持这种输入仍需可证明唯一性的应用级辅助机制。正式 PSI 17 分区包本轮只复验历史干净包格式；未从 `b78e139` 重新生成正式规模包。

目标级仿真、RTL 仿真、实体 HPU 执行：**NOT RUN**。编码指令软件执行与宿主机目标 C 联动不能代替实际硬件验证。

## 最终 Git 与发布核对

最终报告提交后还须在服务器运行 `git diff --exit-code cfee951e HEAD -- include/ src/`、两目录树 ID 对照、未跟踪文件检查，并对两条个人仓库远端分支读取完整 SHA。对应结果以本次交付的最终记录为准。
