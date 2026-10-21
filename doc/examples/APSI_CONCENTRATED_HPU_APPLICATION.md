# APSI 无标签 PSI 集中式 HPU 应用

## 1. 客观实现范围

`hpu_bfv_apsi_concentrated_example` 是一份**明确裁剪的 APSI 无标签 PSI
工作负载移植**，不是完整的 APSI 网络协议实现，也不是旧示例那种单一十次
多项式零值测试。

它忠实保留了影响服务器同态计算的 APSI 数据流：

1. CPU 将发送方和接收方元素变成 128-bit OPRF 输出；
2. 接收方执行多哈希函数 Cuckoo 布局并建立索引翻译表；
3. 发送方把每个 OPRF 输出插入其全部候选位置，将 128-bit 值拆为有限域元素；
4. 同一 bundle 中每个 BFV slot 对应一个 sender bin 的匹配多项式；
5. 客户端只加密配置给出的 source powers；
6. HPU 图按真正的 `PowersDag` 生成缺失幂次，执行密文乘法、重线性化和 ModSwitch；
7. HPU 图执行批量明文多项式求值；满足参数条件时使用
   Paterson–Stockmeyer，而不是把中间幂次或求值结果从 SEAL 导入；
8. 客户端从 HPU 软件执行器的实际输出解密，要求一个 item 的全部 felts 都为零，
   再通过 Cuckoo 索引翻译表恢复交集。

裁剪边界是 OPRF：当前程序使用固定 key 的确定性 128-bit 测试函数模拟
`Receiver::ExtractHashes` 之后的边界，以便 ST 包可复现；它**不是** Microsoft APSI
使用的 FourQ 盲化 OPRF，也不提供 ZeroMQ 消息、恶意对端防护或生产安全性。
因此应称为“APSI-compatible HPU workload”，不能称为完整 APSI 协议。

## 2. 源码对应关系

主要对应 Microsoft APSI：

- `receiver/apsi/receiver.cpp::create_query`：Cuckoo table、bundle 切片、item
  algebraization、source-power 编码和加密；
- `common/apsi/powers.cpp::PowersDag::configure`：按最小深度选择两个父幂；
- `sender/apsi/sender_db.cpp`：sender item 插入所有 hash locations，并按
  `bundle_idx/bin_idx` 组织；
- `sender/apsi/bin_bundle.cpp::BatchedPlaintextPolyn`：每个次数的所有 bin
  系数编码成一个 batched plaintext；
- `sender/apsi/sender.cpp::ComputePowers`：缺失幂、Relinearize、低幂/高幂
  level 选择；
- `BatchedPlaintextPolyn::eval` 与 `eval_patstock`：普通求值与
  Paterson–Stockmeyer 求值，最后降到 last parms level；
- `receiver/apsi/receiver.cpp::process_result_part`：解密并把全零 felt 区间映射回
  receiver item。

Poseidon Trident 的 `apsi/` 版本采用同一数据结构和求值顺序，只把 SEAL
`mod_switch/NTT` 调用替换为 Poseidon 的 `drop_modulus/ntt_fwd` 等接口。本程序的
小规模参数直接取自 Trident `create_params1()`。

## 3. 参数

| 字段 | `small` | `medium` |
| --- | ---: | ---: |
| sender / receiver | 10 / 5 | 8192 / 512 |
| 默认交集 | 2 | 32 |
| N | 8192 | 4096 |
| t | 65537（显式保持 Trident 参数） | 270337（显式保持 APSI 参数） |
| felts/item | 8 | 5 |
| hash functions | 3 | 3 |
| table size | 4096 | 1638 |
| bundle count | 4 | 2 |
| max items/bin | 16 | 125 |
| PS low degree | 0 | 5 |
| source powers | `{1,3,5}` | `{1,2,3,4,5,6,18,30,42,54,60}` |
| 来源 | Trident `create_params1` | APSI `1M-1024-com` 参数结构 |

`medium` 保留官方 `1M-1024-com` 的 table、felt、查询幂和 PS 结构，但把集合规模
缩为 8192/512；它是可执行的代表性工作负载，不是“一百万 sender item”的完整
生产规模测试。

官方参数中的 40/44-bit BFV Q 不满足 HPU Q/P 最多 31 bit 的 ABI。本程序使用
`[31,31,31,31,31]`：四个 data Q 加一个 special P。普通求值把 query powers 从
Q4 降到 APSI 指定的 chain index 1（Q2），求值后降到 Q1；PS 求值把 low powers
降到 chain index 2（Q3），把 high powers 降到 chain index 1（Q2），inner
polynomial 降到 Q2 后与 high power 相乘并重线性化，最后降到 Q1。
没有删除 ModSwitch。

精确的 `t=65537/270337` 由应用直接使用 SEAL 已有的
`EncryptionParameters::set_plain_modulus` 和 `SEALContext` 构造，再把合法 context
交给既有的 `ApplicationBuilder(const SEALContext&)`。本应用没有修改
`BfvContextSpec`、`create_bfv_context()` 或任何 HPU 公共接口。

## 4. CPU / HPU 边界

CPU：集合生成、确定性 OPRF-output fixture、Cuckoo hashing、sender DB
polynomial preprocessing、BatchEncoder、source-power 明文幂和加密、最终解密与
索引恢复。

HPU 单包：所有 bundle 的 missing powers、Square/Multiply、Relinearize、
MultiplyPlain、Add/AddPlain、普通或 PS 求值以及全部 required ModSwitch。

原始 HPU Add 只接受两分量 BFV ciphertext。PS 的 `inner * high_power` 每次产生
三分量 ciphertext 后立即通过既有 `relinearize` 变回两分量，再执行 Add 归约。
这与先对多个三分量 tensor 相加、最后统一重线性化数学等价，但不依赖任何新增
三分量 Add、Codegen、Relocation 或软件执行器能力。

`ApplicationBuilder::compile` 对图中**每个节点**运行独立 SEAL oracle 和 HPU
BFV software executor，并逐 RNS 系数比较。客户端恢复只读取
`compiled.software_result`，不会拿 `oracle_result` 冒充 HPU 输出。

## 5. 构建、测试和交付

```bash
cmake -S . -B build-sealpir -DHPU_ENABLE_SEAL_INTEGRATION=ON
cmake --build build-sealpir -j --target hpu_bfv_apsi_concentrated_example

# 四种小规模语义场景
./build-sealpir/hpu_bfv_apsi_concentrated_example --profile small --scenario empty
./build-sealpir/hpu_bfv_apsi_concentrated_example --profile small --scenario single
./build-sealpir/hpu_bfv_apsi_concentrated_example --profile small --scenario multi
./build-sealpir/hpu_bfv_apsi_concentrated_example --profile small --scenario boundary

# 生成并校验一个集中式包
cmake --build build-sealpir --target bfv_apsi_concentrated_small_delivery
cmake --build build-sealpir --target bfv_apsi_concentrated_medium_delivery
./build-sealpir/hpu_validate_package outputs/bfv_apsi_concentrated_small/hpu_query
```

包目录除通用的 ASM、`.inst32/.cmd26`、DMA、HPU_MEM、逐节点 golden、计算图和
oracle report 外，还包含：

- `pipeline_manifest.json`：CPU/HPU 边界、算子计数、参数、资源和验证状态；
- `set_test_data.json`：输入集合、CPU 交集与 HPU 输出恢复交集；
- `README.txt`：复现和验证命令。

`test/apsi_concentrated_delivery_test.py` 还记录整个生成子进程的 wall time 与 Linux
peak RSS。软件模型和 host 指令回放通过不等于真实 HPU/RTL ST 通过；只有取得目标
硬件执行、内存回读、FAULT/IRQ 和外部 monitor 证据后才能更新硬件状态。

## 6. 2026-10-21 软件实测

测试主机为 Ubuntu 24.04。下表是默认关闭资源复用优化器时，生成在
`outputs/bfv_apsi_concentrated_{small,medium}` 中的交付包数据；一个 profile 的
所有 bundle 都在同一个 HPU 计算图和应用包内。

| 指标 | `small` 重构前 | `small` 重构后 | `medium` 重构前 | `medium` 重构后 |
| --- | ---: | ---: | ---: | ---: |
| HPU 应用包 | 1 | 1 | 1 | 1 |
| graph nodes | 244 | 244 | 286 | 292 |
| encoded instructions | 452775 | 452775 | 296737 | 301321 |
| DMA spans | 179317 | 179317 | 117141 | 118953 |
| HPU_MEM lines（256 B/line） | 303110 | 303110 | 179208 | 179976 |
| compile + software verify | 9.697 s | 9.638 s | 4.583 s | 4.476 s |
| delivery process wall time | 13.126 s | 12.972 s | 6.841 s | 6.685 s |
| delivery process peak RSS | 2376860 KiB | 2376720 KiB | 1491836 KiB | 1509096 KiB |
| 最低输出 noise budget | 7 bit | 7 bit | 5 bit | 5 bit |
| 实际最高 bin 多项式次数 | 1 | 1 | 28 | 28 |

`small` 的 sender 只有 10 个元素，实际 bin 次数因此只有 1；它仍会按配置执行
PowersDag、幂次生成和四种集合语义回归，但不用于证明 PS 分支。`medium` 的实际
次数为 28，确实执行 `ps_low_degree=5` 的 Paterson–Stockmeyer 分支。

`small` 不走 PS 分支，因此重构前后图结构和资源计数不变。`medium` 每个 PS 高项
改为立即重线性化，Relinearize 从 30 次增至 36 次，因而增加 6 个节点、4584 条
指令、1812 个 DMA span 和 768 条 HPU_MEM line；噪声预算仍为 5 bit，包数仍为 1。
这里不把包数不变或局部资源变化表述为端到端性能结论。

实际回归包括旧简化 PSI、二维集中式 SealPIR、四种 APSI 小规模场景、APSI 包
生成/校验，以及恢复到基准版本的 BFV context/evaluator 测试，均通过。`small`、
`medium` 两个 `hpu_query` 均通过 `hpu_validate_package`。这些结果只覆盖 SEAL
oracle、HPU 软件执行器和 host 编码指令模型，`real_hardware_verified` 保持
`false`。

底层目录相对 `8b3cbbf` 的逐文件 Git blob 证明和复现命令见
[APSI PSI HPU 底层零改动审核报告](APSI_CONCENTRATED_HPU_DIFF_AUDIT.md)。
