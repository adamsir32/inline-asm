# PIR/PSI 上游集成与计算语义验证（2026-10-26）

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

固定 Trident `e3f819f845f3cee7425fa08a3aa64879cfd5b3c0` 在独立目录
`/home/zhaozhengyi/independent-pir-psi-audit-20261026/trident-faithful-reference`
构建。Poseidon 与 Kuku、Flatbuffers、jsoncpp 单独安装在该目录；
Trident PIR `pir_bfv` 原生示例完成一次默认随机数据库运行，退出码 0，
日志 `trident-pir-native.log` 报告 `PIR result correct!`。这证明原生参考可运行，
**不是**与 HPU 相同数据库的逐阶段差分。
APSI 的 `apsi` 原生静态库也编译成功，日志 `trident-apsi-build.log`。
为了绕过参考工程硬编码的 `/usr/bin/flatc`，只在独立参考副本修改
`apsi/CMakeLists.txt` 一行路径，记录于 `trident-apsi-build-only.patch`；
未改 APSI 算法函数。另在参考副本附加向量驱动目标，待运行结果单独记录。

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
服务器连接恢复后须重建并重新运行。当前没有目标规模通过结论。

本轮待完成的门槛：原生向量与相同输入对拍、精确模式全部历史反例和随机差分、
两模式应用包验证、PIR 同输入阶段差分、1M/1024 目标规模、
`git diff --exit-code cfee951e4056660e521d55a3535d82335a49bb08 HEAD -- src include encode`
以及最终提交 SHA。此处不把旧报告的测试记录算作新实测。
