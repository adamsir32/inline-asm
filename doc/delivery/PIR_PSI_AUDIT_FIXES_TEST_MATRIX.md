# PIR/PSI 审查修复测试矩阵

执行机：`zhaozhengyi@10.102.104.22:9322`。代码固定为生产者 `2622761537c7dcd73a89da3482da1d65c2b9c9b5`、Nexus-AM `c738b84e1fb7a267c3042bad4144d1ae794b54f3`。完整参数数组、工作目录和退出码在 `audit-fixes-clean-final4/summary.json`。

| 层次 | 测试/步骤 | 预期 | 实际 | 证据 |
|---|---|---|---|---|
| 构建/包 | `configure`：干净 CMake 配置 | 0 | `0` | `logs/configure.log` |
| 构建/包 | `build`：生产者 Release 构建 | 0 | `0` | `logs/build.log` |
| 软件/宿主 | `producer_related_ctest`：生产者相关 CTest 22/22 | 0 | `0` | `logs/producer_related_ctest.log` |
| 软件/宿主 | `pir_dynamic`：PIR Stage 0→CPU→Stage 1 双输入/哨兵/负例 | 0 | `0` | `logs/pir_dynamic.log` |
| 软件/宿主 | `psi_empty`：PSI 空交集 | 0 | `0` | `logs/psi_empty.log` |
| 软件/宿主 | `psi_single`：PSI 单交集 | 0 | `0` | `logs/psi_single.log` |
| 软件/宿主 | `psi_multi`：PSI 多交集 | 0 | `0` | `logs/psi_multi.log` |
| 软件/宿主 | `psi_boundary`：PSI 边界 | 0 | `0` | `logs/psi_boundary.log` |
| 构建/包 | `build_08_01_psi_single`：build_08_01_psi_single | 0 | `0` | `logs/build_08_01_psi_single.log` |
| 构建/包 | `build_08_02_pir_stage0`：build_08_02_pir_stage0 | 0 | `0` | `logs/build_08_02_pir_stage0.log` |
| 构建/包 | `build_08_03_pir_stage1`：build_08_03_pir_stage1 | 0 | `0` | `logs/build_08_03_pir_stage1.log` |
| 构建/包 | `build_08_04_pir_unified`：build_08_04_pir_unified | 0 | `0` | `logs/build_08_04_pir_unified.log` |
| 软件/宿主 | `pir_cpu_encoded`：PIR CPU 分解、编码与固定指令重放 | 0 | `0` | `logs/pir_cpu_encoded.log` |
| 软件/宿主 | `pir_target_main_host`：PIR 统一目标 C 宿主联动 | 0 | `0` | `logs/pir_target_main_host.log` |
| 软件/宿主 | `psi_target_main_host`：PSI 目标 C 宿主联动 | 0 | `0` | `logs/psi_target_main_host.log` |
| 负例 | `package_and_dynamic_negatives`：包与动态输入损坏拒绝 | 0 | `0` | `logs/package_and_dynamic_negatives.log` |
| 负例 | `pir_bad_query`：pir_bad_query | 非零（拒绝） | `1` | `logs/pir_bad_query.log` |
| 负例 | `pir_bad_database_seed`：pir_bad_database_seed | 非零（拒绝） | `1` | `logs/pir_bad_database_seed.log` |
| 负例 | `psi_bad_scenario`：psi_bad_scenario | 非零（拒绝） | `1` | `logs/psi_bad_scenario.log` |
| 负例 | `pir_execution_timeout`：pir_execution_timeout | TIMEOUT（预算拒绝） | `TIMEOUT` | `logs/pir_execution_timeout.log` |
| 负例 | `psi_execution_timeout`：psi_execution_timeout | TIMEOUT（预算拒绝） | `TIMEOUT` | `logs/psi_execution_timeout.log` |

## 补充回归与独立验证

| 层次 | 命令要点 | 结果 | 证据 |
|---|---|---|
| PIR 完整动态交接 | `pir_dynamic_stage_handoff_test.py --record-index 0` | PASS，退出码 0 | `audit-fixes-pir-boundary-0.log`、`audit-fixes-pir-boundary-0/handoff_summary.json` |
| PIR 完整动态交接 | `pir_dynamic_stage_handoff_test.py --record-index 9` | PASS，退出码 0 | `audit-fixes-pir-boundary-9.log`、`audit-fixes-pir-boundary-9/handoff_summary.json` |
| PIR 完整动态交接 | `pir_dynamic_stage_handoff_test.py --record-index 127` | PASS，退出码 0 | `audit-fixes-pir-boundary-127.log`、`audit-fixes-pir-boundary-127/handoff_summary.json` |
| PSI Kuku 两根碰撞 | `ctest -R hpu_bfv_apsi_small_collision-(first|second|miss)` | 3/3 PASS | `audit-fixes-psi-collision-final.log` |
| PSI C→客户端密文 | `test_psi_ciphertext_handoff.py`，最终干净包 | host=0、client=0、corrupt=1 | `audit-fixes-psi-handoff-final4/summary.json` |
| Nexus-AM Python | `python3 -m unittest discover -s tests/hputest/scripts -p test_*.py` | 181 项，2 skipped，退出码 0 | `audit-fixes-nexus-unittest-final.log` |
| Stage 1 独立导入 | `build-pir-psi.py --case 08_03_pir_stage1`，最终干净包 | 退出码 0 | `audit-fixes-stage1-probe.log` |
| 完整 SHA-256 | `sha256sum -c checksums.sha256` | 1,638/1,638 PASS，退出码 0 | `audit-fixes-clean-final4/checksums-verify.log` |
| 附加证据 SHA-256 | `sha256sum -c PIR_PSI_AUDIT_FIXES_EXTRA_SHA256.txt`（在证据根目录） | 15/15 PASS，退出码 0 | `audit-fixes-extra-checksums-verify.log` |
| 受保护目录 | `git diff --exit-code <base> HEAD -- include src encode`；Nexus `tests/hputest` 之外 | 0 差异 | 仓库 Git 树 |
| 实机/RTL | 无 | NOT RUN | 不作硬件通过声明 |

主验收入口：`tests/hputest/scripts/run-pir-psi-small-acceptance.py`。该脚本的 21 个步骤含精确命令与参数，记录于机器可读的 `summary.json`。预期拒绝的非零返回和 TIMEOUT 不计为失败。
