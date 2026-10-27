# PIR/PSI 与 Trident 的计算语义映射

本文记录修改应用前的源码基线和后续验证状态。合并提交为
`063ee45f56a33553ba9bbe13d1f166ce3bf73fff`；其父提交分别为本 fork
`c98b8ef3633de77e0ec724793a0a98ed68532910` 和上游
`cfee951e4056660e521d55a3535d82335a49bb08`。参考检出为 Trident
`e3f819f845f3cee7425fa08a3aa64879cfd5b3c0` 及 Microsoft SealPIR
`cf55f10be7bffba4e78d5ba32764284ac828d6cd`。本 fork 的上游核心目录
`src/`、`include/`、`encode/` 在合并提交中与上游提交逐文件一致。

## 1. 参数与可比性

| 系统 | 数据规模与编码 | BFV/递归参数 | 结论 |
| --- | --- | --- | --- |
| SealPIR 示例 `src/main.cpp:17-28` | 65,536 条 × 1,024 B，`N=4096`，`logt=20`，二维 | 启用递归模数切换 | HPU full 应用直接采用此业务规模及 9 条/明文、86×85 的布局。 |
| Trident PIR `pir_bfv/main.cpp:23-35` | 65,536 条 × 288 B，`N=8192`，`logt=20`，二维 | 示例关闭递归模数切换 | 算法族相同，默认参数、记录字节数和模数链不同；不能把现有 HPU full 的结果称为这个具体默认用例的逐参数复现。 |
| 现有 HPU PIR `examples/bfv_sealpir_2d_application.cpp:25-28,68-75,625-639` | small 128；full 65,536 条 × 1,024 B，`N=4096`，20 有效位 | 三个 31-bit 数据 Q 加 special P，第一阶段后切换至两个 Q | HPU 模数 ABI 适配需按明文/解密语义证明，不能比较随机密文原始字节。 |
| 新增 HPU PIR `--profile trident` | 65,536 条 × 288 B，`N=8192`，20 有效位；每记录 116 个系数，每明文 70 条，937 个明文填充为 31×31 | 同一 HPU 31-bit Q/P 适配及递归模数切换 | 业务数据规模、字节布局和二维索引与 Trident 默认实例相同；中间模数链仍不同，须分别检验软件输出。 |
| Trident APSI `apsi/parameters/1M-1024-com.json` | 表长 1638、三哈希、每 bin 最大 125、5 felt；文件名所指 1M/1024 工作量 | `N=4096`，18 位字段配置，PS 低阶 5，11 个查询源幂，Poseidon 系数模数位宽 `[48,36,25]` | 该 JSON 是 APSI 参数形状，不规定本 fork 减小的数据集规模。 |
| 现有 HPU PSI `examples/bfv_apsi_concentrated_application.cpp:292-302,714-751` | small 10/5；medium 8192/512，medium 的表/字段/幂参数仿 `1M-1024-com` | 5 个 31-bit 模数（四 Q、一 special P） | 数据规模、系数模数、令牌和哈希都未与 Trident 实际路径相同。 |

`1M-1024-com.json` 规定明文参数而不含本程序的 sender/receiver 数量；当前 medium
代码明确写着“sender/receiver counts reduced”。将 medium 的随机通过结果外推到
1M/1024 不成立。Trident 的 `PSIParams::initialize`
(`apsi/common/apsi/psi_params.cpp:141-171`) 将字段数乘以每字段位数，并以
`N / felts_per_item` 确定每 bundle 的元素数；本应用 medium 使用相同的后者公式。

## 2. PIR 逐步骤映射

| 步骤 | SealPIR 参考 | Trident 参考 | HPU 应用及公开接口 | 数据语义、差异与验证 |
| --- | --- | --- | --- | --- |
| 参数和二维索引 | `src/pir.cpp:49-94`，`src/pir_client.cpp:115-122` | `pir_bfv/pir.cpp:49-95`，`pir_bfv/pir_client.cpp:150-160` | `dimensions`、`compute_indices` (`:127-165`) | `plain_index=floor(record/elements_per_plaintext)`，`offset=record mod elements_per_plaintext`，再按矩阵行优先分解。HPU full 的 7282 个实际明文填入 86×85。需逐索引核对。 |
| 数据库字节/批处理 | `src/pir_server.cpp:51-125`，`src/pir.cpp:151-185` | `pir_bfv/pir_server.cpp:80-174`，`pir_bfv/pir.cpp` 编码工具 | `bytes_to_coefficients`、`build_database` (`:188-268`)，SEAL `BatchEncoder` | 每记录 1024 B 用 410 个 20-bit 系数；9 条/明文，空位及矩阵 padding。HPU 测试字节生成器不同于两个参考示例的随机数据库，比较规则应是同一输入字节而非 RNG 轨迹。 |
| 压缩查询 | `src/pir_client.cpp:72-112` | `pir_bfv/pir_client.cpp:91-148` | `:673-698`，SEAL `Encryptor` | 每维在目标 slot 写入 `2^ceil(log2(dim))` 的模逆后加密。随机密文不能按字节比；可解密比较明文 slot。 |
| Galois 元素与查询展开 | `src/pir_server.cpp:303-396` | `pir_bfv/pir_server.cpp:422-533` | `record_expand_query` (`:297-357`)；`Evaluator::rotate_rows`、`add`、`multiply_plain`、`negate_inplace` | 参考使用 `N/2^i+1` 的 Galois 元素、`X^(2N-2^i)` 和旋转后的相应 shift；HPU 把元素换算为公开 `rotate_rows` 步数，再做相同加法/单项式乘法。非 2 的幂最后一层有 corner 分支。应解密逐展开项比较。 |
| 第一维点积 | `src/pir_server.cpp:175-303` | `pir_bfv/pir_server.cpp:222-420` | `record_dot_product_range` (`:360-385`)；`multiply_plain`、`add_inplace`、`mod_switch_to_next_inplace` | 数据库按 `database[k+j*output_count]` 取列，产生每个第二维位置的密文。HPU 可与 SEAL 逐系数/解密明文对拍。 |
| 中间密文分解 | `src/pir.cpp:285-333` | `pir_bfv/pir.cpp:334-397` | `decompose_to_plaintexts` (`:428-452`) | 按组件、模数 limb、20-bit chunk 展开。HPU 首阶段软件验证后取 `oracle_result` 代理作第二阶段输入，不是实体 HPU 前一阶段结果的端到端传递。 |
| 第二维点积与递归解码 | `src/pir_server.cpp:175-303`，`src/pir_client.cpp:174-264` | `pir_bfv/pir_server.cpp:222-420`，`pir_bfv/pir_client.cpp:210-275` | 第二次 `concentrated_stage_hpu`、`decode_recursive_reply` (`:483-520,700-749`) | 第二维对分解明文再展开/点积/切换；客户端解密、重组密文、最终按 offset 取记录。验证必须比较全部 1024 字节。 |

PIR 原应用的跨阶段代理受 `verified_oracle_result` (`:125-138`) 守护：只有软件
SEAL 参考值、HPU 软件执行及主机指令验证成功才提供 SEAL 密文。它验证两张图的
计算语义，不能作为真实 HPU 硬件的端到端实测。Trident 示例默认禁用递归模数切换，
因此该默认实例与 HPU/SealPIR 实例的中间密文层级不可直接认作相同。

## 3. PSI 变更前的逐步骤映射与已知反例

| 步骤 | Trident 的 APSI 路径 | 当前 HPU 应用 | 实际差异/验证办法 |
| --- | --- | --- | --- |
| 原始元素与 OPRF | `apsi/receiver/apsi/receiver.cpp` 中 OPRF 请求/响应；`apsi/common/apsi/oprf/` 的 FourQ 实现 | `test_oprf` (`:117-124`) 用确定性 `splitmix64` 造两字令牌 | 只可称 CPU 夹具，未运行真实 OPRF。要比较真实 Trident 令牌须从参考路径产生固定向量。 |
| 候选位置和 Cuckoo | `Receiver::create_query` (`receiver.cpp:219-278`) 构建种子 `{0,0}` 的 `KukuTable`，500 次尝试；`SenderDB::all_locations` (`sender_db.cpp:43-54`) 用 Kuku `LocFunc` | `hash_location`、`all_locations`、`build_cuckoo_layout` (`:126-199`) 使用自定义 splitmix64 | 表长与哈希数相同不表示位置相同。需要同一 128-bit HashedItem 的 Kuku 位置向量及布局对照。 |
| felt 编码 | `PSIParams::initialize` (`psi_params.cpp:141-171`) 决定 90 bit；`algebraize_item` (`util/db_encoding.cpp:354-360`) 用 `bits_to_field_elts` | `algebraize` (`:145-163`) 依序取低 90 bit | 两者均只进入 90 bit；需以实际参考向量核对位序。完整 128-bit 令牌高 38 bit 不进入这组多项式。 |
| sender 数据库/根 | `SenderDB` (`sender_db.cpp:136-168,196-293`) 在每候选位置插入 felt；`BinBundle` (`bin_bundle.cpp:987-1060`) 构造各 slot 匹配多项式，容量满时可建立更多 BinBundle | `build_sender_coefficients` (`:381-417`) 每位置、每 felt 独立建根；单一 bundle、bin 超过 125 即拒绝 | 低负载时形式相似，1M sender 目标会触发多 BinBundle 差异；当前应用无法表示全部参考结果包。 |
| 幂依赖与多项式求值 | `Sender::compute_powers` (`sender.cpp:233-333`)，`BatchedPlaintextPolyn::eval/eval_patstock` (`bin_bundle.cpp:103-343`) | `configure_powers_dag`、`GraphRecorder`、`evaluate_standard/evaluate_patstock` (`:259-284,419-618`) | 源/目标幂及 PS 低阶同形；HPU 因公开接口限制对乘积立即重线性化，模数链也不同。应比较解密后每 slot 的有限域值，而非密文表示。 |
| 客户端判定 | `Receiver::process_result_part` (`receiver.cpp:490-505`) 检查连续 `felts_per_item` 个零值 | `:868-881` 逐 felt 零值 AND 后恢复原始 item | 两者形式相同，均有跨 sender felt 拼接的概率性误报。数值对拍不证明精确集合交集。 |

Trident 的 `PSIParams::log2_fpp` (`psi_params.h:142-152`) **明确把匹配定义为具有
误报概率**：近似 `5·(-18+log2(125))≈-55.17` bit/接收方元素。上一阶段的
定向反例（见独立验证报告）把这种小概率组合构造为确定发生：真实
`uint64_t` 发送方 5 项、接收方 `1000000`，标准交集为空而五个 slot 均为零。
因此：

1. 按 Trident 这份 APSI 源码逐字实现，可与其**概率性**结果对齐，但不能证明
   “每个接收方的 128-bit 令牌均精确匹配”，也不能保证旧反例消失。
2. 只增加 felt 数量可覆盖被截断的令牌位，但仍不能消除跨 sender 的量词交换。
3. 用户要求旧误报反例回归为不匹配，这比该 APSI 参数的固有保证更强；若公开
   HPU API 和现有模数链无法在目标规模内表达按**同一个 sender 元素**关联的
   多字段相等判定，必须报告能力/资源缺口，不能事后按 CPU 正确交集过滤结果。

Kuku 参考版本 `1338c4ae2211ab4c739022ff57f48ce5a76531d5` 的
`src/kuku/kuku.cpp::generate_loc_funcs` 从零种子逐个递增，
`src/kuku/internal/hash.h::HashFunc` 使用 BLAKE2xb 初始化 16×256 个随机表项；
本应用的 splitmix64 候选位置算法显然不同。当前集合交集正确性依赖于发送方
和接收方使用同一个应用哈希，不代表与 Trident 的具体候选位置相同。

## 4. 验证关口和未证结论

语义层：同一数据库/集合输入上，对照 SealPIR/Trident 参考的明文布局、查询展开、
中间结果和最终字节/集合；随机密文不比较原始字节。编译链层：对每个 HPU 图检查
SEAL 逐系数、软件执行、编码指令模型和包合法性。硬件层：只有实体 HPU/RTL 执行
才可以标记通过。任何目标参数测试未运行、真实 OPRF/哈希向量未取得、或跨阶段
代理未替换为实体 HPU 输出时，分别标记未验证。

本页是实施前映射；后续代码修改、同输入参考结果和残留差异需要在最终验证报告
中逐项更新，不能用现有 small/medium PASS 代替目标配置证明。

## 5. 应用层修复后的 PSI 语义

`examples/bfv_apsi_concentrated_application.cpp` 现在把每个 128-bit 令牌按低位在前
切成八个 16-bit（small）或 18-bit（medium/target）felt，后者的末尾补零。
每个候选位置的 sender 元素按首 felt 不重复且每组最多 125 项分区。
对分区 `p`，令 `x_s` 为 sender 元素首 felt、`y_{s,j}` 为第 `j` 个其余 felt：

\[
F_p(X)=\prod_{s\in p}(X-x_s),\qquad
L_{p,j}(x_s)=y_{s,j}\quad (1\le j\le7).
\]

sender 在 CPU 侧计算匹配多项式及七个拉格朗日插值多项式。receiver 对自己的
`x` 加密查询源幂，并另外加密七个 `y_j`，按两个 BFV 批处理组排列。
HPU 用现有 `Evaluator` 的乘法、加法、重线性化、模数切换和减法求出
`F_p(x)` 与七个 `L_{p,j}(x)-y_j`；客户端仅在**同一分区**的八个值全部为零时
报告匹配。`F_p(x)=0` 强制 `x` 等于分区内某一 sender 根；根在该分区互异，
所以全部插值值都来自同一个 sender 元素。八个 felt 覆盖全部 128 bit，
从而在该令牌编码及域参数下消除了截断与跨 sender 拼接误报。
`small` 的 `t=65537` 和 `medium/target` 的 `t=270337` 均为素数；
非零插值分母可用费马逆元。sender 分区构造保证同分区的首 felt 互异。
空 bin 的匹配多项式为常数 1；容量和首 felt 冲突产生额外 sender 分区，
客户端检查所有分区。这个方案只调用上游已有公开接口。
在 `target` 规模，每个 sender 分区生成并验证独立的 HPU 应用包，所有包复用
同一组客户端加密查询；客户端对各包解密后求匹配结果的并集。
`small`/`medium` 保持所有分区在一个包内。拆包只改变计算图的资源边界，
不会用 CPU 交集替换任何包的密文判零结果。

这是一项为满足**精确集合交集**要求而作的应用语义增强，**不等同于**
Trident APSI `BinBundle` 的五个独立 felt 多项式。因此对普通输入，
两者的集合交集通常相同；对已经构造出的概率性误报反例，
两者刻意不同。不能称为 Trident 算法逐算子复现。
当前 OPRF 是确定性 CPU 测试夹具，候选位置仍用本应用的 splitmix64 哈希；
没有真实 FourQ OPRF 或 Kuku 同输入位置对照。目标参数入口 `--profile target`
使用 1,000,000 sender/1,024 receiver、Trident 配置中的表长、哈希数、
最大 bin 项数、PS 分界和查询源幂；令牌/哈希与 Trident 协议仍有差异。

## 6. PIR 默认 Trident 规模的应用入口

`--profile trident` 将本应用的记录大小和多项式次数设为
Trident `pir_bfv/main.cpp:23-35` 的 288 B 与 8192，并保持 65,536 条、
`logt=20`、二维查询。源代码仍复用同一查询展开、二维点积、分解和恢复路径；
不是以代码名称近似替代算法。由 `ceil(288·8/20)=116` 个明文系数/条、
`floor(8192/116)=70` 条/明文可得 937 个有效明文；Trident
`pir_bfv/pir.cpp::get_dimensions` 与本应用相同的整数平方根展开规则给出
`31×31`。应用在运行时断言全部这些派生量。

该入口继续使用 HPU 支持的四个 31-bit 系数模数，并在第一阶段后进行
模数切换。Trident 默认示例不执行递归模数切换；因此业务记录可按完整字节
比较，但中间密文的模数层级不能宣称相同。应用的确定性数据库生成器也不同于
Trident 示例随机数据库；外部字节 Oracle 从相同数据库种子独立生成期望记录。

## 7. 第二阶段双模式 PSI（2026-10-27）

本节从受保护提交 `7a060f6c0e6aa191606babc2e9d236969d8e5a1f` 出发；
以上第 3、5 节是此前只有精确模式的历史说明。
`--match-mode exact` 仍为默认，保留八字段同一 sender 插值、splitmix64
候选位置和完整令牌精确匹配。
`--match-mode trident` 对应固定 Trident
`e3f819f845f3cee7425fa08a3aa64879cfd5b3c0` 的无标签 APSI 计算：

| 原生函数 | 新应用表达 | 对齐边界 |
| --- | --- | --- |
| `PSIParams::initialize`、`bits_to_field_elts` | `bit_count(t)-1` 位/字段，低位在前；medium/target 为 5×18=90 bit | 字段编码仍须与原生向量逐项比较。 |
| `Receiver::create_query` | Kuku 2.1 的零种子 `KukuTable`、500 次探测、无 stash | 原始 128-bit 令牌决定位置；随机驱逐使具体位置不总是固定。 |
| `SenderDB::all_locations`、`insert_or_assign_worker` | Kuku `LocFunc(table_size, make_item(i,0))`；逆序选择可容纳的 BinBundle；新 bin 长度严格 `< max_items_per_bin` | 线程调度会影响合法分包顺序。 |
| `BinBundle::regen_polyns` | 各 felt 独立计算 `∏(X-r)`；空槽为常数 1 | 没有七个同 sender 插值标签，保留 APSI 概率性匹配。 |
| `PowersDag`、`ProcessBinBundleCache` | 同源幂与 PS 目标幂集合，HPU 公开乘法/平方/立即重线性化/模数切换/多项式求值 | HPU 的 31-bit 模数链属于参数适配。 |
| `Receiver::process_result_part` | 同一 BinBundle 连续字段全零；跨包取并集 | 原始集合交集单独记录，验收比较 APSI 参考匹配。 |

目标规模逐 sender 分区出包只调整资源边界，保留每个 BinBundle 的多项式和
判零语义。两个模式的包清单都标记 `match_mode`、`match_strategy`。
当前两模式的 OPRF 输入仍为确定性 CPU 夹具；`--test-input` 支持注入同一
128-bit 令牌。CPU 根成员模型不等于 Trident 原生端到端结果。

PIR 本轮审查未发现必须修复的 D 类核心语义错误，故保留应用算法。
`rotate_rows` 对 Galois 置换属 A 类等价表达；31-bit 模数链与显式递归
切换属 B 类 HPU 适配；测试数据库生成方式属 C 类外围差异。
不同模数链下逐阶段明文对照仍属 E 类，最终字节 Oracle 不证明中间密文一致。
