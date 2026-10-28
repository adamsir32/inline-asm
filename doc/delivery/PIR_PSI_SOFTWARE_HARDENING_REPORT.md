# PIR/PSI 软件收尾审计

基线：`7afc64ec3245c0fdba5a8bb8512cd744ba6680e2`；工作分支：`feature/trident-pir-psi-hardware-readiness`。原作者核心 `src/`、`include/`、`encode/` 未修改。本文仅报告本轮实际检查；旧规模产物见[硬件就绪报告](PIR_PSI_HARDWARE_READINESS.md)。

## 已修复

| 问题 | 根因与处理 | 计算图影响 |
|---|---|---|
| 自定义 PSI 输入重复 | `load_test_input` 原来无条件追加发送方和接收方行，使重复发送方元素形成额外根。现按原始 item 对各方分别稳定去重；相同 item 的冲突令牌、不同 item 的相同完整令牌均明确拒绝。后者在注入令牌的测试边界上有歧义：原始集合不同而 Trident 实际按令牌匹配。部分字段相同不视为相同令牌。 | 默认生成数据完全不变；自定义重复输入的根/查询数恢复为集合语义。 |
| SenderDB 差分空跑 | 没有 `*.raw` 或 `--full` 名称不存在时，旧驱动循环为空仍返回 0。现在配置错误记入 `summary.json` 并退出非零；摘要记录输入、执行、完整 HPU 验证数量及每例异常。 | 无。 |
| PIR 外部读回缺少入口 | 原应用始终使用 `CompiledApplication::oracle_result`。新增可选 `--stage0-readback <完整执行后 HPU_MEM 镜像>`：按包的输出 span 读取小端 word，逐 word 对 Golden，逐 RNS limb 做 BR→自然系数转换，覆盖密文全部数据，然后原有 CPU 分解及第二阶段编译接收该数据。缺失、截断、数据变异均失败。 | 默认路径和图不变；提供读回时第二阶段的输入数值随读回变化，仍要重新编译。 |

## 回归与防假通过

- `test/apsi_trident_duplicate_input_test.py`：正常集合、双方重复、冲突令牌、不同 item 同一完整令牌、空集、缺接收方和非法行。重复案例的预处理 dump 与正常案例逐字相同，证明去重发生在发送方多项式建立前。
- 直接把重复原始行送入隔离参考的 `apsi_native_senderdb` 审计驱动时，驱动自身报 `duplicate raw input item` 并退出 1（`../readiness-duplicate-native-diff/duplicate.native.log`）；因此这个驱动不能用于重复输入的端到端原生差分。这里的正向证据是 HPU 规范化后与无重复基线 dump 相同，不能把驱动拒绝误写为 SenderDB 计算失败。
- `test/apsi_trident_senderdb_differential_self_test.py`：空目录、不存在的 `--full`、案例执行异常均必须失败并留机读摘要。
- `test/sealpir_stage0_readback_test.py`：使用**明确标记的合成镜像**测试外部读回适配器、第二阶段记录恢复、损坏和截断拒绝。合成数据取自包 Golden，不作为独立执行读回证据。
- 检查 `apsi_concentrated_delivery_test.py`、`sealpir_2d_delivery_test.py`、`apsi_trident_cross_felt_test.py`、`apsi_trident_native_differential.py`：子进程均核返回码；交付测试还核真实记录或恢复集合和软件验证字段。未发现另一个可复现的漏检。`apsi_trident_senderdb_differential.py` 完整模式同时核原生结果、`AUDIT_RECOVERED` 数字列表和软件验证状态，非仅 grep PASS。PSI 五字段判零是 Trident 语义，跨字段假阳性不能用 CPU 原始集合交集替代。

## 本轮执行

服务器工作目录：`/home/zhaozhengyi/independent-pir-psi-audit-20261026/trident-alignment-20261026/repo`。构建目录：同级 `build/`。

| 命令/范围 | 状态 | 证据 |
|---|---|---|
| `cmake -S . -B ../build -DBUILD_TESTING=ON`；构建两个应用 | PASS | `../readiness-reconfigure.log`、`../readiness-pir-build.log`；PSI 编译控制台成功 |
| `ctest --test-dir ../build -R 'hpu_bfv_apsi_(trident_duplicate_input\|senderdb_driver_failures)\|hpu_bfv_sealpir_2d_concentrated_small_record_(0\|64\|127)$'` | PASS，5/5 | `../readiness-ctest.log` |
| 应用相关 CTest（小规模 PIR、PSI、自定义输入、合成读回） | PASS，13/13 | `../readiness-app-ctest.log` |
| `ctest --test-dir ../build -R 'hpu_bfv_sealpir_2d_concentrated_small_record_(8\|9)$'` | PASS，2/2；相邻明文边界 | `../readiness-boundary-ctest.log` |
| SenderDB 13 例预处理差分 | PASS，13/13，完整 0 | `../readiness-senderdb-differential.log`、`../readiness-senderdb-differential/summary.json` |
| SenderDB 13 例差分，`--full single` | PASS，13/13，完整 1 | `../readiness-senderdb-full-single.log`、同名目录 `summary.json` |
| 合成 PIR 读回适配器 | PASS；**不是**独立读回 | `../readiness-readback-adapter.log` |
| 最终 PIR 代码 CTest：0/8/9/64/127 与读回适配 | PASS，6/6 | `../readiness-pir-final-ctest.log` |

本轮没有重新生成正式规模包。现有 19 个包是静态审计对象，其来源字段仍标示旧提交且 `dirty-at-configure`；不能把本轮新分支代码与旧包声明为同一构建。真正独立的第一阶段执行后内存读回尚缺，见[阶段交接报告](PIR_STAGE_HANDOFF_VALIDATION.md)。

上述 PASS 命令的退出码均为 **0**；预期拒绝的重复原生 raw、空目录/错 `--full` 和损坏读回则为**非零**。应用测试所用代码树在首个提交 `115c3debb81a77fd7a6d3fe156701bfe2e953c6a` 中保存；本报告后续文字变更不改变可执行代码。完整可复现命令（在服务器 `repo/` 目录运行）：

```sh
cmake -S . -B ../build -DBUILD_TESTING=ON
cmake --build ../build --target hpu_bfv_apsi_concentrated_example hpu_bfv_sealpir_2d_concentrated_example -j4
ctest --test-dir ../build -R 'hpu_bfv_sealpir_stage0_readback_adapter|hpu_bfv_sealpir_2d_concentrated_small_record_(0|8|9|64|127)$' --output-on-failure
python3 test/apsi_trident_senderdb_differential.py \
  --native ../../trident-faithful-reference/trident-apsi-build/apsi/bin/apsi_native_senderdb \
  --params ../../trident-faithful-reference/trident/apsi/parameters/1M-1024-com.json \
  --example ../build/hpu_bfv_apsi_concentrated_example \
  --case-dir ../../trident-faithful-reference/senderdb-cases \
  --output ../readiness-senderdb-full-single --full single
python3 test/pir_psi_package_audit.py \
  --pir ../finalization-pir-target --psi ../finalization-psi-target \
  --output doc/delivery/PIR_PSI_PACKAGE_MANIFEST.csv
```

包校验以每个目录执行 `../build/hpu_validate_package <package-directory>`，19 条实际退出码见逐包 TSV。`../readiness-senderdb-full-single/summary.json` 明列 `input_case_count=13`、`run_case_count=13`、`full_hpu_verified_count=1`。原生重复 raw 失败是审计驱动在进入 SenderDB 前的输入限制，不计入正常案例失败。
