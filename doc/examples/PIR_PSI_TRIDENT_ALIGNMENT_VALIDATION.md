# PIR/PSI 上游集成与计算语义验证（2026-10-26）

> **最终状态以文末“2026-10-28 收尾验收”及 [交付指南](TRIDENT_PIR_PSI_FINAL_GUIDE.md) 为准。**
> 以下旧章节是按日期保留的历史记录，其中 exact 模式、未验证状态和旧版
> PSI `t=270337` 指标均不代表收尾分支。固定 Trident JSON 经原生
> `PSIParams::Load` 实际得到 `t=188417`、每 felt 17 位、五 felt 共 85 位；
> 收尾分支已修正应用参数并重新验证。已验证提交 `96837ad` 在前一轮已推送到
> 用户 fork 的 `feature/trident-faithful-pir-psi`；收尾分支按用户后续授权
> 推送至 `adamsir32/inline-asm`，上游仓库未修改。

前半部分记录 2026-10-26 第一阶段的当时状态；2026-10-27 的双模式与原生
同输入实测见文末“第二阶段独立验证记录”。前半部分的“尚未验证”不可
代替第二阶段结论。

## 版本与工作区

| 项目 | SHA / 状态 |
| --- | --- |
| fork 原开发分支与备份 | `c98b8ef3633de77e0ec724793a0a98ed68532910` |
| 上游 `feature/operator-resource-management` | `cfee951e4056660e521d55a3535d82335a49bb08` |
| 两者共同基线 | `7306d1c69010b3e92576a19f1d90b1c25cf01f8c` |
| 独立集成分支合并提交 | `063ee45f56a33553ba9bbe13d1f166ce3bf73fff`，双亲为上述 fork 和上游提交 |
| Trident 参考 | `e3f819f845f3cee7425fa08a3aa64879cfd5b3c0` |
| SealPIR 参考 | `cf55f10be7bffba4e78d5ba32764284ac828d6cd` |

开发与软件运行位于 SSH 服务器的独立目录
`/home/zhaozhengyi/independent-pir-psi-audit-20261026/trident-alignment-20261026`。
fork 原开发分支没有合并本集成分支，也没有向 GitHub 推送。
合并的冲突只涉及 `README.md`；`CMakeLists.txt` 保留上游目标行为。
合并后及应用修改后，
`git diff cfee951e4056660e521d55a3535d82335a49bb08 -- src include encode`
均无输出，即上游核心目录的内容与正式上游提交相同。

## 修改归属

| 文件 | 目的 |
| --- | --- |
| `examples/bfv_apsi_concentrated_application.cpp` | 用同一 sender 的七个插值字段验证完整令牌；支持 sender 分区及 1M/1024 入口；保留回归输入开关。 |
| `examples/bfv_sealpir_2d_application.cpp` | 为独立字节 Oracle 提供确定性数据库种子与实际记录输出；新增 65,536×288 B、`N=8192` 的 Trident 参数入口，保留原有 small/full 行为。 |
| `CMakeLists.txt` | 只增加本 fork 的 PIR Trident 参数、PSI 目标规模交付入口和反例回归注册。 |
| `test/fixtures/apsi_exact/` | 保存已知误报、容量边界及 medium 真实测试令牌输入。 |
| `test/apsi_concentrated_delivery_test.py` | 验证目标规模的多个 sender 分区应用包。 |
| `test/apsi_random/` | 固定种子随机差分数据生成器、独立 CPU 模型与 HPU 运行脚本。 |
| `test/sealpir_2d_delivery_test.py`、`test/pir_record_oracle.py` | 对交付的 PIR 记录执行独立字节生成与比较。 |
| `doc/examples/APSI_CONCENTRATED_HPU_APPLICATION.md` | 区分旧概率性版本的历史测量与当前精确匹配计算图。 |
| `doc/examples/SEALPIR_2D_HPU_APPLICATION.md` | 记录新增 PIR Trident 参数及仍然存在的模数链差异。 |
| `doc/examples/PIR_PSI_TRIDENT_SEMANTIC_ALIGNMENT.md` | 源码步骤映射、参数出处、修复证明与未解差异。 |
| 本报告 | 版本、执行和限制证据。 |
| `README.md` | 链接算法映射与本验证报告。 |

以上修改仅涉及 fork 自有应用、应用文档、夹具和所需构建注册。
未修改 HPU `Evaluator`、编译器、指令模型、软件执行器或公共接口。

## 算法判断

PIR 的原 `full` 入口继续使用 SealPIR 的 65,536 条 × 1,024 B、`N=4096`
二维递归计算。新增 `trident` 入口使用 Trident `pir_bfv/main.cpp` 默认示例的
65,536 条 × 288 B、`N=8192`，并核对每条 116 个 20-bit 系数、每明文 70 条、
937 个有效明文填入 31×31 的索引布局。Trident 默认关闭递归模数切换，
本应用继续采用 HPU 31-bit Q/P 链并切换，因此中间模数层级不同；
软件最终字节一致不证明中间密文表示逐参数相同。
PIR 两阶段之间使用经 SEAL/HPU 软件逐系数一致性验证的 SEAL 密文代理，
不构成实体硬件端到端传递。

原 PSI 只检验五个低位 felt，且每个 felt 可来自不同 sender。
修复后的每个 sender 分区建立首 felt 匹配多项式和七个同 sender 字段插值多项式。
客户端对同一分区同时检查八个零值；插值定义与根互异保证八个字段都属于
**同一个** sender 令牌。八字段覆盖 128 bit。超过 125 项或首 felt 冲突时建立
附加分区；126 项不再被整体拒绝。详见[语义映射](PIR_PSI_TRIDENT_SEMANTIC_ALIGNMENT.md)。

这个精确匹配扩展满足本任务的反例回归要求，但与 Trident APSI
五字段独立多项式的概率性误报语义不完全相同。Trident 自身在
`apsi/common/apsi/psi_params.h::log2_fpp` 对误报概率作了定义。
当前输入令牌与哈希位置由本应用的确定性测试函数及 splitmix64 生成，
没有执行 FourQ OPRF 或 Kuku 的同输入参考对照。

## 软件验证记录

服务器配置为 Release/Ninja，`HPU_ENABLE_SEAL_INTEGRATION=ON`，
依赖 GSL、zlib、zstd 使用服务器已缓存的源目录。构建
`hpu_bfv_sealpir_2d_concentrated_example`、
`hpu_bfv_apsi_concentrated_example` 与 `hpu_validate_package` 通过。
每次应用运行都要求逐节点 SEAL oracle、HPU 软件执行器及 host 编码指令模型
验证成功，然后才解密最终代理输出；业务结果还与原始集合或记录比较。
在服务器集成目录下复现的核心命令为：

```bash
cmake -S repo -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DHPU_ENABLE_SEAL_INTEGRATION=ON \
  -DFETCHCONTENT_SOURCE_DIR_MSGSL=/home/zhaozhengyi/independent-pir-psi-audit-20261026/deps/GSL-152d6eb989a1ecd23fe9c9cfb2fb8cfc7c0cd0c1 \
  -DFETCHCONTENT_SOURCE_DIR_ZLIB=/home/zhaozhengyi/independent-pir-psi-audit-20261026/deps/zlib-da607da739fa6047df13e66a2af6b8bec7c2a498 \
  -DFETCHCONTENT_SOURCE_DIR_ZSTD=/home/zhaozhengyi/independent-pir-psi-audit-20261026/deps/zstd-f8745da6ff1ad1e7bab384bd1f9d742439278e99
cmake --build build --target hpu_bfv_sealpir_2d_concentrated_example \
  hpu_bfv_apsi_concentrated_example hpu_validate_package -j 4
ctest --test-dir build -R 'apsi_concentrated|hpu_bfv_apsi_exact' --output-on-failure
ctest --test-dir build -R 'sealpir_2d_concentrated_small' --output-on-failure
cmake --build build --target bfv_apsi_concentrated_medium_delivery
cmake --build build --target bfv_sealpir_2d_concentrated_trident_delivery
build/hpu_bfv_apsi_concentrated_example --profile target
```

回归测试的输入位于 `test/fixtures/apsi_exact/`，其中 genuine token
输入会额外执行 `--test-require-fixture-tokens` 检查。
PIR 外部字节对照脚本为 `test/pir_record_oracle.py`；PIR 交付测试也会调用
此脚本的独立字节生成函数验证 `actual_record.bin`。

| 验证 | 实测结果 | 服务器证据 |
| --- | --- | --- |
| 合并后的旧 PSI 真实令牌反例 | 预期空集，实际误报 `1000000`，基线失败 | `psi-real-baseline.log`、`psi-real-medium-baseline.log` |
| 修复后 small / medium 同一反例 | 空集，应用与三层软件对照通过；medium 最低噪声 4 bit | `psi-real-exact-small.log`、`psi-real-exact-medium.log` |
| 原有 PSI small 四场景与包交付 | CTest 5/5 通过 | `exact-ctest.log` |
| 新 PSI 对抗回归 | CTest 6/6 通过：截断、跨字段、真实令牌、125/126 容量 | `exact-regression-ctest.log` |
| medium 随机差分 | 五组覆盖交集 0、1、32、256、500，应用与三层软件对照均通过 | `psi-random-00.log` 至 `psi-random-04.log` |
| PSI 分区逐包版随机差分 | 20 组固定种子 medium 输入，交集大小 0、1、32、256、500 各四组；应用、独立集合交集及三层软件对照 20/20 通过 | `psi-random-streaming.log`、`psi-random-streaming-cases/summary.json` |
| PSI 资源优化 | medium 默认交集 32，优化开启后仍正确，SEAL/HPU/指令模型通过 | `psi-medium-optimized.log` |
| PIR small 索引与包交付 | CTest 4/4 通过 | `pir-small-ctest.log` |
| PIR SealPIR full 末索引 | 65,536×1,024 B，两个阶段 SEAL/HPU 通过；实际 1,024 B 与外部 Oracle 1/1 字节一致 | `pir-run/pir-full-65535.log`、`.time`、`pir-full-oracle.json` |
| PIR Trident 参数末索引 | 65,536×288 B、`N=8192`、31×31，两个阶段 SEAL/HPU 通过；实际 288 B 与外部 Oracle 1/1 字节一致 | `pir-trident-run/pir-trident-65535.log`、`.time`、`pir-trident-oracle.json` |
| PIR Trident 参数边界索引 | 0、69、70、65534、65535 五条记录均通过 SEAL/HPU 软件验证；外部字节 Oracle 5/5 完全一致 | `pir-trident-run/pir-trident-{0,69,70,65534,65535}.log`、`pir-trident-oracle-all.json` |
| PIR Trident 参数交付包 | 两个应用包均由 `hpu_validate_package` 接受，交付脚本的独立字节 Oracle 通过 | `pir-trident-delivery.log` |
| PSI 分区逐包版回归 | 11/11 通过：原有场景、交付包及全部六项反例；small/medium 仍生成一个包 | `psi-streaming-ctest.log` |
| PSI 分区逐包版 medium 交付 | 一个应用包，SEAL/HPU/主机指令模型及包验证均通过；实测 21.041 秒，峰值 RSS 1,646,732 KiB | `psi-streaming-medium-delivery.log` |
| PSI 分区逐包版资源优化开关 | 同一 medium 输入的优化前后交集均为 32、最低噪声均为 4 bit，SEAL/HPU/指令模型均通过；指令数由 168,471 降至 122,362 | `psi-medium-streaming-unoptimized.log`、`psi-medium-streaming-optimized.log` 及相应 `.time` |
| PSI 目标入口多包夹具 | 126 项 sender、1 项 receiver，两个分区包；正确恢复 1 项交集，两包均由 `hpu_validate_package` 接受 | `psi-target-capacity-126.log`、`target-capacity-126-validate-{0,1}.log` |
| PSI 1M/1024 目标入口交付 | 1,000,000 sender、1,024 receiver，17 个分区应用包逐包校验通过；预期和恢复交集均为 32 项，最低噪声 3 bit，逐节点 SEAL/HPU 软件/主机指令模型通过。外层计时 2,430.22 秒，峰值 RSS 5,686,960 KiB | `psi-target-streaming-delivery.log`、`psi-target-streaming.time`、`repo/outputs/bfv_apsi_concentrated_target/pipeline_manifest.json` |
| 上游 BFV 相关测试 | context、基本/乘法计划、密文乘法 codegen、Evaluator 共 5/5 通过 | `upstream-bfv-build.log`、`upstream-bfv-ctest.log` |
| 最终同步后应用回归 | 重新配置及构建通过；PIR small、PSI small、交付和六项反例合计 CTest 15/15 通过，0 失败；随机差分构建目标已注册 | `final-build.log`、`final-application-ctest.log` |

`--profile target` 使用 1,000,000 sender / 1,024 receiver 和 Trident
`1M-1024-com.json` 的表、bin 容量、源幂及 PS 分界。它与真实 Trident
OPRF/哈希路径仍不同。最初将所有 sender 分区编进同一包的版本在 3,600 秒
限制处超时（退出码 124，峰值 RSS 35,662,316 KiB）；它没有通过目标规模验证。
应用现改为在目标规模逐分区编译/验证多包，沿用同一份加密查询并对解密匹配
取并集。实测 17 包、22,918 个图节点、13,556,657 条编码指令，全部包合法性
校验通过，交集与原始集合一致；最初失败证据为 `psi-target.log`、
`psi-target.time`，本次成功证据见上表。该结果证明当前自定义令牌和位置函数下
的目标规模 HPU 软件计算，不等同于 Trident 原生 FourQ/Kuku 路径对拍。

## 明确未验证的结论

- PIR 的 Trident 默认 288 B / `N=8192` 参数及最终字节已在 HPU 应用实测；
  真实 Trident 二进制对同一数据库的逐阶段数值对拍尚未完成。用户确认服务器
  没有现成参考程序或向量；检出的 Trident `pir_bfv/CMakeLists.txt` 链接
  `poseidon_shared`，该参考仓库顶层构建没有提供相应目标。
- PSI 的 FourQ OPRF、Kuku 位置、Trident 实际 BinBundle 输出尚未在同一输入上运行；
  当前只证明了本应用精确令牌交集和 HPU 现有软件执行链的一致性。参考 APSI
  构建还要求 Kuku、Flatbuffers、jsoncpp 等依赖，服务器没有现成可执行程序。
- 所有结果均为 SEAL 对照、HPU 软件执行及主机指令模型；没有实体 HPU/RTL 实测。
- 未合并到 fork 目标开发分支，也未推送 GitHub。

## 最终判定

1. 上游已在独立集成分支安全合并，原开发分支和备份点保留；
   `src/`、`include/`、`encode/` 与上游提交完全一致。
2. PIR 的 SealPIR/Trident 业务参数、二维布局和最终记录字节在 HPU 软件路径上
   得到验证；Trident 的原生逐阶段运行、不同模数链的中间等价以及实体硬件
   端到端传递尚未证明，故不宣称完整 Trident 实现等价。
3. PSI 的完整 128-bit 令牌精确交集、反例回归、随机差分和 1M/1024 规模
   均在当前 HPU 软件路径上通过；它使用与 Trident APSI 不同的匹配多项式、
   OPRF 和位置函数，故不宣称完整 Trident 协议或同输入算子语义对齐。
4. 上述通过项是实际软件测试；关于 Trident 对应关系的其余内容是源码分析，
   应在获得可运行参考路径及同输入向量后继续验证。没有真实 HPU/RTL 执行。

## 第二阶段独立验证记录（2026-10-27；进行中）

本阶段从已推送的受保护提交
`7a060f6c0e6aa191606babc2e9d236969d8e5a1f` 建立服务器分支
`feature/trident-faithful-pir-psi`，另建
`backup/pre-trident-faithful-20261027` 指向相同提交。
上游基线仍为 `cfee951e4056660e521d55a3535d82335a49bb08`。
未向 GitHub 推送，未合并目标开发分支。

相对受保护提交，本轮修改或新增的文件为：`CMakeLists.txt`、
`examples/bfv_apsi_concentrated_application.cpp`、
`test/apsi_concentrated_delivery_test.py`、
`test/apsi_trident_counterexample_search.cpp`、
`test/apsi_trident_cross_felt_test.py`、
`test/apsi_trident_native_differential.py`、
`test/apsi_trident_native_vector.cpp`、
`test/fixtures/apsi_trident/README.md`、
`test/fixtures/apsi_trident/cross_felt_native.input`、
`test/fixtures/apsi_trident/native_input.txt`、
`test/pir_trident_native_main.py`、
`doc/examples/APSI_CONCENTRATED_HPU_APPLICATION.md`、本语义映射文档及本报告。
没有新增底层算子，也没有修改公共 HPU 接口。

固定 Trident `e3f819f845f3cee7425fa08a3aa64879cfd5b3c0` 在独立目录
`/home/zhaozhengyi/independent-pir-psi-audit-20261026/trident-faithful-reference`
构建。Poseidon 与 Kuku、Flatbuffers、jsoncpp 单独安装在该目录；
Trident PIR `pir_bfv` 原生示例完成一次默认随机数据库运行，退出码 0，
日志 `trident-pir-native.log` 报告 `PIR result correct!`。这证明原生参考可运行，
**不是**与 HPU 相同数据库的逐阶段差分。
APSI 的 `apsi` 原生静态库也编译成功，日志 `trident-apsi-build.log`。
为了绕过参考工程硬编码的 `/usr/bin/flatc`，只在独立参考副本修改
`apsi/CMakeLists.txt` 一行路径，记录于 `trident-apsi-build-only.patch`；
未改 APSI 算法函数。另在参考副本附加向量驱动目标；其后续运行结果见下文。

| 本轮命令（服务器集成目录 `repo`） | 退出码 | 结果与日志 |
| --- | ---: | --- |
| `cmake -S . -B ../build -DCMAKE_PREFIX_PATH=.../trident-faithful-reference/reference-prefix` | 0 | 发现 Kuku 2.1.0；`../trident-faithful-configure.log`。 |
| `cmake --build ../build --target hpu_bfv_apsi_concentrated_example -j4` | 0 | `../trident-faithful-build.log`。 |
| `../build/hpu_bfv_apsi_concentrated_example --match-mode trident --profile small --scenario multi` | 0 | 原始/参考/恢复匹配均 2；SEAL、HPU 软件及指令模型通过；244 节点、2891 指令、1161 DMA、23025 HPU_MEM 行、最低噪声 7 bit；`../trident-small-multi.log`。 |
| `../build/hpu_bfv_apsi_concentrated_example --match-mode trident --profile medium --scenario multi` | 0 | 三种匹配均 32；三层软件验证通过；301 节点、107094 指令、40691 DMA、119025 HPU_MEM 行、最低噪声 5 bit；`../trident-medium-multi.log`。 |
| `../build/hpu_bfv_apsi_concentrated_example --match-mode exact --profile small --scenario multi` | 0 | 历史模式仍通过；288 节点、7515 指令、3121 DMA、50673 HPU_MEM 行、最低噪声 7 bit；`../exact-small-multi.log`。 |
| `../build/hpu_bfv_apsi_concentrated_example --match-mode trident --profile target --scenario multi` | 未取得退出码 | 运行期间 SSH 连接在服务端握手前关闭；不可计为通过。`../trident-target-multi.log` 待恢复后检查。 |

前三个应用运行的 `SEAL per-node oracle`、HPU 软件执行和主机编码指令模型均报告
PASS，未运行实体 HPU/RTL。`trident` 的 CPU APSI 参考目前基于同一组 BinBundle
字段根独立计算成员关系，属于独立 CPU 模型等级；在原生同输入向量完成前，
不称为 Trident 原生端到端对照。

目标规模初次运行使用旧构建，每个 sender 元素都重新初始化 Kuku `LocFunc`。
源代码已改为每进程缓存与 `SenderDB::hash_functions` 相同的三个位置函数；
服务器连接恢复后已重建并重新运行；当时尚无目标规模通过结论，后续结果见下文。

当时待完成的门槛：原生向量与相同输入对拍、精确模式全部历史反例和随机差分、
两模式应用包验证、PIR 同输入阶段差分、1M/1024 目标规模、
`git diff --exit-code cfee951e4056660e521d55a3535d82335a49bb08 HEAD -- src include encode`
以及最终提交 SHA。此处不把旧报告的测试记录算作新实测。

### SSH 恢复后的新增实测

服务器分支已快进到本地实现提交 `e15a66624aef81c6857086da82e1b2e93b56fe51`。
快进前的两个服务器未提交文件另存为
`trident-alignment-20261026/pre-sync-worktree.patch` 和 stash
`pre-sync-trident-faithful-20261027`；新分支工作区干净。

| 执行命令摘要 | 退出码 | 证据 |
| --- | ---: | --- |
| `cmake -S repo -B build -DCMAKE_PREFIX_PATH=.../reference-prefix`，随后构建 PSI 应用及 `hpu_validate_package` | 0 / 0 | `trident-faithful-configure-latest.log`、`trident-faithful-build-latest.log`。 |
| `ctest -R 'hpu_bfv_apsi_(exact\|trident)_small\|hpu_bfv_apsi_trident_small_multi\|hpu_bfv_apsi_concentrated_small_'` | 0 | 10/10，含 exact 历史截断、跨字段、真实令牌反例及 trident 小型反例；`trident-faithful-small-ctest.log`。 |
| `ctest -R 'hpu_bfv_apsi_exact_medium_'` | 0 | 3/3，含真实令牌、125/126 容量边界；`trident-faithful-exact-medium-ctest.log`。 |
| `python3 test/apsi_random/run_apsi_random_differential.py --example ../build/hpu_bfv_apsi_concentrated_example --out-dir ../trident-faithful-exact-random`（默认 20 组） | 0 | 每组 8192 sender / 512 receiver；独立集合答案、HPU exact 恢复结果 20/20 一致；`trident-faithful-exact-random.log`、`trident-faithful-exact-random/summary.json`。 |
| `ctest --test-dir ../build --output-on-failure -R 'hpu_bfv_sealpir_2d_concentrated_small_record_|bfv_sealpir_2d_concentrated_small_delivery_test'` | 0 | PIR small 边界及交付 4/4 通过；`trident-faithful-pir-small-ctest.log`。 |
| `apsi_concentrated_delivery_test.py --profile small --match-mode trident ...` | 0 | 单包通过上游验证器；`trident-small-delivery.log`、`trident-small-delivery/pipeline_manifest.json`。 |
| `python3 test/apsi_concentrated_delivery_test.py --example ../build/hpu_bfv_apsi_concentrated_example --validator ../build/hpu_validate_package --output ../exact-small-delivery --profile small --scenario multi --match-mode exact` | 0 | 单包通过上游验证器；`exact-small-delivery.log`、`exact-small-delivery/pipeline_manifest.json`。 |
| `apsi_concentrated_delivery_test.py --profile medium --match-mode trident ...` | 0 | 单包通过上游验证器；`trident-medium-delivery.log`、`trident-medium-delivery/pipeline_manifest.json`。 |
| 固定 Trident APSI `bits_to_field_elts`、`polyn_with_roots`、`PowersDag` 与原生 Kuku 向量驱动，及 `apsi_trident_native_differential.py` 同输入运行 | 0 / 0 | 字段、Kuku 候选位置及接收方放置、幂次 DAG 节点、根多项式系数逐项一致；`trident-faithful-reference/trident-native-vector.log`、`trident-alignment-20261026/trident-native-diff/{native,hpu}.log`。这属于真实原生函数向量对照，不是 Trident 完整协议运行。 |
| 原生 Kuku 搜索驱动产生 `cross_felt_native.input`，同一输入分别运行 `--match-mode trident` 与 `exact` | 0 / 0 | Kuku 公共位置 1410；原始集合交集空，trident APSI 参考及解密恢复均为 `{3}`，exact 参考及恢复均为空；两者三层软件验证通过。日志 `trident-faithful-reference/trident-cross-felt-hpu.log`、`exact-cross-felt-hpu.log`。 |

原生向量驱动只调用固定参考的算法函数，独立副本的 CMake
增加一个可执行目标和私有链接目录；第三方算法源码没有改动。
`cross_felt_native.input` 的值来自固定 Kuku 2.1.0 的可重复搜索程序，
证明 `trident` 的误匹配符合其逐 felt 独立根机制，不能归因于 HPU 计算错误。
1M/1024 目标规模已在恢复后的构建重新启动；完整结果见下表。

small/multi 同一输入的包清单给出算子数量对照。两模式都使用 44 次密文乘法、
8 次平方和 52 次重线性化；`trident` / `exact` 分别使用 4 / 8 次密文乘明文、
0 / 8 次密文减法、4 / 8 次明文加法、132 / 160 次模数切换。
相应的节点数为 244 / 288，编码指令 2891 / 7515，DMA 1161 / 3121，
HPU_MEM 行 23025 / 50673，最低噪声均为 7 bit。
这些是软件编译/验证得到的工作负载数量，不是实体 HPU 性能。

| 目标规模实测（1,000,000 sender / 1,024 receiver） | 退出码 | 结果与证据 |
| --- | ---: | --- |
| `python3 test/apsi_concentrated_delivery_test.py --example ../build/hpu_bfv_apsi_concentrated_example --validator ../build/hpu_validate_package --output ../trident-target-delivery --profile target --scenario multi --match-mode trident` | 0 | 原始交集、APSI 参考匹配和 HPU 恢复均为 32；17/17 个包通过上游验证器；SEAL 逐节点、HPU 软件执行及最终交集、主机指令模型均 PASS。17 包、12593 节点、8742583 指令、3321031 DMA、最大包 5980417 HPU_MEM 行，最低输出噪声 3 bit；编译加软件验证 782.386 秒，整项墙钟 15:36.04，峰值 RSS 3810624 KiB。日志 `../trident-target-latest.log`、`../trident-target-latest.time`；真实 HPU/RTL 未运行。 |
| `python3 test/apsi_concentrated_delivery_test.py --example ../build/hpu_bfv_apsi_concentrated_example --validator ../build/hpu_validate_package --output ../exact-target-latest-delivery --profile target --scenario multi --match-mode exact` | 0 | 原始交集、exact 参考匹配和 HPU 恢复均为 32；17/17 个包通过上游验证器；SEAL 逐节点、HPU 软件执行及最终交集、主机指令模型均 PASS。17 包、22918 节点、13556657 指令、5116973 DMA、最大包 10171877 HPU_MEM 行，最低输出噪声 3 bit；编译加软件验证 2152.635 秒，整项墙钟 39:53.31，峰值 RSS 5717296 KiB。日志 `../exact-target-latest.log`、`../exact-target-latest.time`；真实 HPU/RTL 未运行。 |

目标规模的算子计数由两个包清单给出：

| 模式 | 密文乘法 | 平方 | 重线性化 | 密文乘明文 | 加法 | 减法 | 明文加法 | 模数切换 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `trident` | 1017 | 102 | 1119 | 3992 | 3959 | 0 | 33 | 2371 |
| `exact` | 1648 | 102 | 1750 | 7974 | 7908 | 66 | 66 | 3404 |

这是各自算法工作负载的实测差异；不能把 `exact` 的额外开销归于
Trident 原始 APSI。墙钟时间也仅代表当前服务器的软件编译和验证速度。

### PIR 同数据库原生对照

隔离参考副本的 `pir_bfv/main.cpp` 由
`test/pir_trident_native_main.py` 生成确定性输入/记录输出夹具，原件保存在
`pir_bfv/main.cpp.unmodified`，完整差异保存在
`trident-faithful-reference/trident-pir-main-only.patch`。
未修改 `pir.cpp`、`pir_client.cpp` 或 `pir_server.cpp`。该夹具使用与 HPU 应用
完全相同的 65,536×288 B 数据库字节生成公式及索引输入；Trident 保持原生
Poseidon 模数参数和默认关闭递归模数切换，HPU 保持 31-bit Q/P 适配。

固定 Trident 原生程序对五个索引均退出码 0，报告恢复正确；原生日志分别为
`trident-faithful-reference/trident-pir-same-input-65535.log` 和
`trident-alignment-20261026/trident-pir-other-records/native_{0,69,70,65534}.log`。
HPU 三次直接运行分别查询 `0`（附加检查 `69`）、`70`、`65534`
（附加检查 `65535`），均退出码 0、SEAL oracle 与 HPU 软件执行 PASS，
日志 `trident-pir-other-records/hpu_{0,70,65534}.log`。
附加检查的 69 和 65535 输出来自实际 HPU 计算，不是 CPU 数据库拷贝。
另对索引 65535 单独运行
`sealpir_2d_delivery_test.py --profile trident --record-index 65535`，退出码 0，
两个应用包均被上游验证器接受，日志
`trident-alignment-20261026/trident-pir-same-input-hpu-65535.log`。

| 索引 | 原生与 HPU 288 字节输出 `cmp` 退出码 | 两者共同 SHA-256 |
| ---: | ---: | --- |
| 0 | 0 | `83af4fc22d0af683f404633fc20d262fc4c040efc7eab4e8cf54ba63e102fb93` |
| 69 | 0 | `3796430a42605b34fb8fae2ee97f7cf003b68c1a7989d715276e1b00a1298c61` |
| 70 | 0 | `0ff4fb6f0607f6d1dfd33d1648b3052ef199f3665cf1a774b945eb6f85be67e8` |
| 65534 | 0 | `e5f164896a30d6e79c1a8ea9a21298453da871f6db964012230755ea8b1a5196` |
| 65535 | 0 | `5a22fb801e8addf789d7a4009b6b2cf57d106994570b9060ef56da2d236d1a6d` |

这证明相同业务输入的最终记录对齐，不证明不同模数链下中间密文表示相同。

## 2026-10-28 收尾验收

本节记录独立收尾分支 `feature/trident-pir-psi-finalization` 上的**本轮实测**；前面各节为保留的历史记录。受保护的起点是 `96837ad64523fdd181df699c416a7a22d23363db`，原 fork 长期分支 `7a060f6c0e6aa191606babc2e9d236969d8e5a1f` 未改动。上游固定基线 `cfee951e4056660e521d55a3535d82335a49bb08`，Trident 参考 `e3f819f845f3cee7425fa08a3aa64879cfd5b3c0`。收尾代码先在本地提交；用户随后明确授权推送到自己的 fork，本轮不合并、不创建 PR。

### 原生预处理发现和应用修正

隔离 Trident 参考副本中，审计驱动调用实际 `SenderDB::insert_or_assign`，仅为读取私有 `bin_bundles_` 缓存而给该副本 `sender_db.h` 增加只读访问器；`SenderDB` 插入、Kuku、`BinBundle` 和多项式算法源均未改写。访问器源为 `test/apsi_trident_senderdb_accessor.py`，原件及补丁在服务器 `trident-faithful-reference/trident/apsi/sender/apsi/sender_db.h.unmodified` 和 `trident-faithful-reference/senderdb-audit-accessor.patch`。`test/apsi_trident_senderdb_native.cpp` 使用固定合法 FourQ OPRF 标量 1 产生发送方令牌；跨字段反例的接收方在 OPRF 输出边界注入，且其 Kuku 实际摆放位置由原生表查询确认。

原生 `PSIParams::Load` 对固定 `1M-1024-com.json` 实际给出 `t=188417`、17 bit/felt、85 bit/item，JSON 的 SHA-256 为 `ac3cb19932e4122819164a0db1c7ada934bd445538615a798e29893f3c5cfa3d`。先前应用的 `t=270337` 是**真实的参数语义错误**：它改变字段根及多项式系数，尽管 Kuku 位置可能仍相同。本轮只在 `examples/bfv_apsi_concentrated_application.cpp` 将 medium/target 参数修正为 188417，并在原生向量工具中使用 85 位。HPU 使用 4096 个批处理槽，原生 BinBundle 为 4095 个槽；多出的 HPU 槽固定为空槽多项式 1，不参与有效表位置。

`test/apsi_trident_senderdb_differential.py` 对每个 BinBundle 的每个非空槽比较根与**原生缓存实际系数**，以完整实例内容的多重集合容忍合法的 BinBundle 顺序变化；还比较令牌字段、Kuku 候选与接收方摆放、幂次 DAG、原生多项式判零。位于 `trident-faithful-reference/senderdb-cases/` 的 13 组原始输入覆盖单元素、不同桶、同桶、部分字段相同、无/单/多匹配、Kuku 边界、30 次 PS、124 项容量上限、125/126 项额外分包及跨字段组合。服务器命令：

```bash
python3 test/apsi_trident_senderdb_differential.py \
  --native ../../trident-faithful-reference/trident-apsi-build/apsi/bin/apsi_native_senderdb \
  --params ../../trident-faithful-reference/trident/apsi/parameters/1M-1024-com.json \
  --example ../build/hpu_bfv_apsi_concentrated_example \
  --case-dir ../../trident-faithful-reference/senderdb-cases \
  --output ../../trident-faithful-reference/senderdb-final-differential \
  --full single shared_bin partial_felt ps_degree_30 extra_bundle_125 cross_felt
```

退出码 0，13/13 预处理差分通过；列出的 6 组还通过完整 SEAL 逐节点、HPU 软件执行、主机指令模型及原生最终判零对照。`senderdb-final-differential.log` 是逐样例状态，`senderdb-final-differential/summary.json` 是机读汇总，`*.native`/`*.hpu` 是逐槽数据，`*.full.log` 是完整软件运行证据。跨字段样例的原始集合交集为空，而原生和 HPU 均恢复 `{999999}`；这属于 Trident APSI 的概率性假阳性，不能用 CPU 精确交集过滤。另以 `finalization-native-vector-diff.log` 记录原生字段、位置、幂次和合成根多项式向量测试，退出码 0。

### 正式代码和 Kuku 构建隔离

正式 PSI 应用移除了完整令牌插值 exact 分支、第二组期望字段密文、相应的减法图和双模式恢复；保留的 `--match-mode trident` 仅作命令行兼容。`CMakeLists.txt` 新增默认关闭的 `HPU_ENABLE_TRIDENT_PSI`：只有开启时才执行 `find_package(Kuku 2.1 CONFIG REQUIRED)`、构建 PSI 和注册其测试。旧 exact 随机目标、CTest 注册及不再适用的测试源/夹具已从收尾分支移除；两个仍用于 Trident 概率性回归的夹具迁入 `test/fixtures/apsi_trident/`。完整历史均可从受保护提交 `96837ad` 恢复。

无 Kuku 的**干净**目录 `build-no-kuku` 使用 `-DHPU_ENABLE_SEAL_INTEGRATION=ON -DHPU_ENABLE_TRIDENT_PSI=OFF -DCMAKE_DISABLE_FIND_PACKAGE_Kuku=TRUE` 配置并构建 `hpu_bfv_evaluator_example`、`hpu_bfv_sealpir_2d_concentrated_example`、`hpu_validate_package`，退出码均为 0。首次配置因服务器向 GitHub 下载 GSL 时 TLS 失败，随后明确指定已有的 GSL、zlib、zstd 源码缓存，配置和构建成功；`finalization-no-kuku-config.log` 与 `finalization-no-kuku-build.log` 分别记录成功配置和实际编译。成功配置未提供 Kuku 前缀，且主动禁用 Kuku 查找。

同一无 Kuku 构建的 `hpu_bfv_sealpir_2d_concentrated_example --profile small --record-index 0` 也实际运行成功，SEAL 参考和 HPU 软件执行 PASS，退出码 0，日志 `finalization-no-kuku-pir-smoke.log`；该构建的目标列表不存在 `hpu_bfv_apsi_concentrated_example`。有 Kuku 构建中 `--match-mode exact` 以退出码 1 明确拒绝。

有 Kuku 的目录 `build` 使用 `-DHPU_ENABLE_TRIDENT_PSI=ON` 和独立 `reference-prefix` 配置，`finalization-config.log`、`finalization-build.log` 记录退出码 0。全部目标构建 `finalization-all-build.log` 退出码 0。参考前缀安装 Kuku 2.1.0；其参考源码固定提交为 `1338c4ae2211ab4c739022ff57f48ce5a76531d5`。Poseidon 参考 CMake 版本 1.0.0，Trident/Poseidon 等抽取源码档案 `trident-reference-sources.tar.gz` 的 SHA-256 是 `16330368bee6a4d78f20bc22334eab05ab622fe7c9e9305d7c4230c133a5bda1`；Flatbuffers/jsoncpp 等抽取档案 `apsi-native-deps.tar.gz` 的 SHA-256 是 `4ddb5e985250940b941b846b6e3a86af19a3fb656080fa6f693bdf02b35cbe76`。这些抽取档案未保留独立组件的 `.git` 元数据，不能据此声称已核实 Poseidon、Flatbuffers、jsoncpp 的完整 Git 提交；可核验的是固定档案校验值和原生运行结果。

参考依赖源码中的版本字段分别为 Poseidon 1.0.0、Flatbuffers 1.12.0、jsoncpp 1.9.5、spdlog 1.17.0；独立 `spdlog-ref-src.tar.gz` 的 SHA-256 为 `c35509bd585b8f43efe5905d149e4acd8daaca6ab5c230ea267e8e7f0d243d7d`。GSL、zlib、zstd 的源码缓存目录名包含固定提交 `152d6eb989a1ecd23fe9c9cfb2fb8cfc7c0cd0c1`、`da607da739fa6047df13e66a2af6b8bec7c2a498`、`f8745da6ff1ad1e7bab384bd1f9d742439278e99`，用于 modified-SEAL 的干净配置；它们不是 Kuku 依赖。

### 项目回归

迁移 Trident 回归夹具后的**最终文件状态**重新执行 `cmake -S repo -B build -DHPU_ENABLE_TRIDENT_PSI=ON`、`cmake --build build -j4`，均退出码 0；`finalization-final-config.log`、`finalization-final-build.log` 是对应证据。随后 `ctest --test-dir build --output-on-failure -j4` 发现 91 项、通过 91 项、失败 0、跳过 0，退出码 0、墙钟 170.63 秒；`finalization-final-ctest-list.log` 为实际发现清单，`finalization-final-ctest.log` 为逐项结果。PIR small、PSI small 的空/单/多/边界、跨字段、应用包以及原有 HPU 单元测试均包含在这 91 项中。目标规模不包含在 CTest 默认集合里，单列如下。

### PIR 正式规模与原生记录

本轮实际执行：

```bash
/usr/bin/time -v -o ../finalization-pir-target.time \
  python3 test/sealpir_2d_delivery_test.py \
    --example ../build/hpu_bfv_sealpir_2d_concentrated_example \
    --validator ../build/hpu_validate_package \
    --output ../finalization-pir-target --profile trident --record-index 65535
```

退出码 0。规模 65,536×288 B、`N=8192`，记录 65535 从实际两阶段密文路径恢复；两包分别为 `finalization-pir-target/stage0_concentrated_000` 和 `stage1_concentrated_000`，均通过上游验证器。两包共 2,780 图节点、886,228 编码指令、316,196 DMA 段，最大包 2,016,645 行 HPU_MEM；两阶段最低噪声预算均为 29 bit。SEAL 参考、HPU 软件执行和主机指令模型通过；墙钟 1:57.48，峰值 RSS 6,124,484 KiB。`finalization-pir-target.log` 含数值与逐包验证，`finalization-pir-target.time` 含资源计量。包清单 SHA-256 为 `0c39bd3b91fd95743500ec734aa4b96e389d6db2d873e5ea811d6d255b00b462`。当前 `actual_record.bin` 与独立生成的 `expected_record.bin` 都是 `5a22fb801e8addf789d7a4009b6b2cf57d106994570b9060ef56da2d236d1a6d`，还与**历史** Trident 原生同输入索引 65535 的 288 字节记录相同。

前一轮已对索引 0、69、70、65534、65535 分别运行固定 Trident 原生程序，并与当时的 HPU 软件恢复记录逐字节比较，五组 `cmp` 退出码均为 0，SHA-256 列于上面的“PIR 同数据库原生对照”。本轮没有重复运行这五个原生查询；本轮新运行的是索引 65535 的正式参数交付，并用新恢复文件校验其与旧原生 65535 结果同 SHA。PIR 应用源本轮未修改，故其余四组作为可追溯的历史证据保留，不能表述为本轮重新执行。

PIR `full`（65,536×1,024 B、`N=4096`）的代表性索引 65535 来自**前一轮历史实测** `pir-run/pir-full-65535.log` 和 `.time`：退出码 0，阶段 0/1 最低噪声 27/28 bit，SEAL 和 HPU 软件执行通过，墙钟 3,942.55 秒，峰值 RSS 25,514,816 KiB。本轮 PIR 源码没有改动；为避免重跑同一约 65 分钟工作量，在发现历史实测记录后中止了本轮重复尝试，后者退出码 130、没有完成，记录在 `finalization-pir-full.log`、`.time`，**不算本轮通过**。本轮实际重新完成的正式 PIR 路径是上面的 Trident 参数 65,536×288 B 两包交付。

PIR 交付目录实际有 196 个文件、约 1.1 GiB。服务器证据根目录的 `finalization-pir-target.FILES.tsv` 列出每个相对文件名与字节数，`finalization-pir-target.SHA256SUMS` 给出每个文件的 SHA-256；两份索引自身的 SHA-256 分别是 `ab4aac6fd6e091cd3bc984f1c6dbc51ec32f61bdd89d7e625b840f3c4b088504` 和 `79bcccd2a4b802141ecadf4a72871d5239d0313e40fd84a39ab91a136a3ad3a4`。索引由 `find` 实际遍历并对全部文件运行 `sha256sum` 生成，可在交付目录执行 `sha256sum -c ../finalization-pir-target.SHA256SUMS` 重验。

### PSI 正式目标规模

本轮在修正 `t=188417`、移除 exact 后实际执行：

```bash
/usr/bin/time -v -o ../finalization-psi-target.time \
  python3 test/apsi_concentrated_delivery_test.py \
    --example ../build/hpu_bfv_apsi_concentrated_example \
    --validator ../build/hpu_validate_package \
    --output ../finalization-psi-target --profile target --scenario multi
```

退出码 0。发送方 1,000,000、接收方 1,024；原始交集、APSI CPU 参考、HPU 解密恢复均为 32 项。参数 `N=4096`、`t=188417`、五字段、三 Kuku 函数、表长 1638、两个位置 bundle、17 个发送方 BinBundle 分区；配置上限 125、观察最高多项式次数 124，11 个源幂生成 25 个目标幂。17/17 个实际应用包通过上游包验证器；SEAL 逐节点、HPU 软件执行、主机指令模型均 PASS，实体 HPU/RTL 未运行。总图节点 12,593、编码指令 8,742,583、DMA 段 3,321,031、最大包 HPU_MEM 5,980,417 行；最低输出噪声 4 bit。软件编译加验证 730.907 秒，整个脚本墙钟 14:38.98，峰值 RSS 3,807,472 KiB。`finalization-psi-target.log` 包含各分区数值及 17 次包校验，`finalization-psi-target.time` 记录外层资源；`finalization-psi-target/pipeline_manifest.json` 的 SHA-256 为 `786c423aa6645e43ddbd2d24c6648fc9bc20ec65e648b15fbed5ea9ae15aa887`。

旧版基线为 17 包、12,593 节点、8,742,583 指令、3,321,031 DMA、最低噪声 3 bit。本轮前四项数值不变；**推断**原因是固定输入的 Kuku 摆放、BinBundle 容量、幂次 DAG 与计算图拓扑未随明文字段模数改变。明文模数及字段根的数值已被原生差分证明改变，最低噪声由 3 增至 4 bit，不能把相同节点计数误当作参数未修正。百万级原生 SenderDB 全库逐槽对照本轮未运行；13 组边界输入已做真实原生差分，百万级运行验证的是修正后应用的完整软件交付路径。

PSI 交付目录实际有 392 个文件、约 5.8 GiB；`finalization-psi-target.FILES.tsv` 列全量文件名与字节数，`finalization-psi-target.SHA256SUMS` 列每个文件的 SHA-256。两份索引自身的 SHA-256 分别为 `866ae5f099e9f39b6d8f132b2ba91df6b504c16c91a73a86fcc6a89f8372c086` 和 `ef56e3de92f08f7e960e3cb004a52b28994959b3c3e1e507e9d84fb9acdb15a9`。索引保存在交付目录的同级；在交付目录执行 `sha256sum -c ../finalization-psi-target.SHA256SUMS` 可逐文件重验。

### 证据位置、验证层级和未覆盖范围

服务器源码根目录为 `/home/zhaozhengyi/independent-pir-psi-audit-20261026/trident-alignment-20261026/repo`；上述 `finalization-*.log`、`finalization-*.time`、`finalization-*-target/` 和全量文件索引位于其父目录 `/home/zhaozhengyi/independent-pir-psi-audit-20261026/trident-alignment-20261026/`。原生 Trident 参考及 `senderdb-final-differential/` 位于同级的 `/home/zhaozhengyi/independent-pir-psi-audit-20261026/trident-faithful-reference/`。两个正式交付目录的汇编、编码指令、DMA 和 HPU_MEM 均由当前 HPU 构建链生成，未人工补造；逐文件清单可定位这些产物。

本轮软件验证层级是：SEAL 参考逐节点系数、现有 HPU 软件执行器、主机编码指令模型、上游应用包格式验证器。包验证器通过单独表示结构有效，数值正确性依据前三级及解密结果。实体 HPU、SoC、RTL 均**未运行**。普通应用未执行 FourQ OPRF 交互式网络协议，原生差分只在固定密钥的 OPRF 输出边界审计；不能将软件耗时推算为硬件性能，也不能将概率性 PSI 匹配强制解释为精确集合交集。PIR 的 31 位系数模数适配及经过软件验证的阶段间参考密文代理维持不变。

生成交付时的 HPU PSI 可执行文件 SHA-256 为 `d4f3a87941523826b933fdab4728039aec66448d8ac645941b485113ad0db7d2`，PSI 应用源码 SHA-256 为 `9495f6cd7f6665229e764bcd54ab15fab460e86c493ab4102c2950be98f21d01`；PIR 可执行文件与源码分别为 `a77d2e7c6c67fad793c167a35fe0a3f81e2dcbbb3943bb8ed7d3ed449f2b2184`、`7c56121c8b0ed729cdaea0ebf63d3ffdaf4aee401b5f2b3ff26101554a8dd1dc`。应用包从本轮待提交工作树生成；最终提交后再次校验源码哈希，以证明提交包含运行时的应用代码。包内如有历史基础提交字段，不应误读为本轮实际应用源码版本。

### 上游核心保护与收尾分支变更

服务器备份引用 `backup/trident-faithful-96837ad-20261027` 指向完整 `96837ad64523fdd181df699c416a7a22d23363db`。本轮未触及 `src/`、`include/`、`encode/`；最终提交后运行 `git diff --exit-code cfee951e4056660e521d55a3535d82335a49bb08 HEAD -- src include encode`，退出码 0。没有修改上游公开 API、底层算子、指令语义、DMA 或软件执行器。CMake 仅给 PSI 增加独立开关与限定依赖，原有 HPU/PIR 目标在无 Kuku 干净构建中已真实构建和运行。

与受保护提交 `96837ad` 相比，收尾分支的完整文件变更清单如下；`R` 表示原内容迁移到 Trident 夹具目录：

```text
M CMakeLists.txt
M README.md
M doc/examples/APSI_CONCENTRATED_HPU_APPLICATION.md
M doc/examples/PIR_PSI_TRIDENT_ALIGNMENT_VALIDATION.md
M doc/examples/PIR_PSI_TRIDENT_SEMANTIC_ALIGNMENT.md
A doc/examples/TRIDENT_PIR_PSI_FINAL_GUIDE.md
M examples/bfv_apsi_concentrated_application.cpp
M test/apsi_concentrated_delivery_test.py
D test/apsi_random/README.md
D test/apsi_random/generate_psi_hpu_cases.py
D test/apsi_random/psi_cpu_model.py
D test/apsi_random/run_apsi_random_differential.py
M test/apsi_trident_counterexample_search.cpp
M test/apsi_trident_cross_felt_test.py
M test/apsi_trident_native_vector.cpp
A test/apsi_trident_senderdb_accessor.py
A test/apsi_trident_senderdb_cases.py
A test/apsi_trident_senderdb_differential.py
A test/apsi_trident_senderdb_native.cpp
D test/fixtures/apsi_exact/README.md
D test/fixtures/apsi_exact/capacity_125.input
D test/fixtures/apsi_exact/capacity_126.input
D test/fixtures/apsi_exact/real_input.input
D test/fixtures/apsi_exact/real_input_medium.input
M test/fixtures/apsi_trident/README.md
R test/fixtures/apsi_exact/cross_felt.input -> test/fixtures/apsi_trident/cross_felt.input
M test/fixtures/apsi_trident/cross_felt_native.input
R test/fixtures/apsi_exact/truncation.input -> test/fixtures/apsi_trident/truncation.input
```

本轮尚未解决的是完整 FourQ OPRF 网络协议、实体 HPU/RTL 验收和百万级原生 SenderDB 全库逐槽对照；这些均不属于当前软件移植验收的通过项。原生 SenderDB 的 13 组差分及目标规模应用软件交付已分别完成。收尾分支按用户后续授权仅推送至 `adamsir32/inline-asm`，不推送原作者上游。
