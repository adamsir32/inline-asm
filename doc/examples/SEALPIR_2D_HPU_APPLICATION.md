# Microsoft SealPIR 二维 BFV HPU 应用

入口：`examples/bfv_sealpir_2d_application.cpp`

本示例独立于原有的 `bfv_poseidon_pir_application.cpp`。旧程序仍保留一维查询展开和点积，
新程序则按照 Microsoft SealPIR 的 `src/main.cpp`、`src/pir.cpp`、
`src/pir_client.cpp`、`src/pir_server.cpp` 实现两轮递归。参考版本为
Microsoft SealPIR 提交 `cf55f10be7bffba4e78d5ba32764284ac828d6cd`。

## 参数和布局

| 参数 | 小规模回归 | SealPIR 完整规模 |
|---|---:|---:|
| 记录数 | 128 | 65536 |
| 每条记录 | 1024 bytes | 1024 bytes |
| `N` | 4096 | 4096 |
| 可用明文位数 `logt` | 20 | 20 |
| 维度 `d` | 2 | 2 |
| 每个明文的记录数 | 9 | 9 |
| 有效数据库明文 | 15 | 7282 |
| 二维矩阵 | 4×4 | 86×85 |

数据库严格走 SealPIR 的布局：每条记录先拆为 410 个 20-bit 值，同一明文容纳 9 条记录，
再使用 `BatchEncoder::encode`。有效明文的未用槽填 1；二维矩阵的额外 padding 明文按
SealPIR 原码直接构造为 4096 个系数 1。客户端解密后使用 `BatchEncoder::decode`，按原始
记录在明文内的 offset 恢复并校验完整 1024 bytes。

## 两轮递归对应关系

1. 客户端把记录号换算为 BFV 明文号和明文内 offset，再按行主序计算两个维度的索引。
2. 每个维度生成一个系数选择明文：目标系数为
   `inverse(2^ceil(log2(n_i))) mod t`，随后对称加密。
3. `stage0_expand` 用 HPU `ApplyGalois`、`Add`、乘 `X^k` 和 `Negate` 展开第一维查询。
4. `stage0_dot_shard_*` 用 HPU `MultiplyPlain`/`Add` 计算第一维点积，并执行一次 HPU
   `ModSwitch`。分片只限制单个应用包的峰值内存，不改变各输出之间的数据依赖。
5. CPU 将第一轮 **HPU 软件执行器的实际密文** 按 20-bit 块分解为 BFV 明文。这里不读取
   SEAL oracle；oracle 只用于逐节点对拍。
6. `stage1_expand` 独立展开第二维加密查询。
7. `stage1_dot_shard_*` 以第一轮实际结果的分解明文为数据库，执行第二轮 HPU 点积和
   HPU `ModSwitch`，得到递归回复。
8. 客户端解密回复明文，每 8 个明文重组为一个两分量密文，再次解密、BatchDecode，恢复
   最终记录。

HPU 的 `MultiplyPlain` 内部完成所需的 NTT 域转换，因此图边界仍是普通 BFV 密文和明文；
这与 SealPIR 显式 `transform_to_ntt`/`transform_from_ntt` 的数学语义相同。

## 原版参数与 HPU 适配

SealPIR 的 `BFVDefault(4096)` 使用约 36/36 bit 的数据 Q 和约 37 bit 的特殊 P；当前 HPU
ABI 要求 Q/P 不超过 31 bit。最初按相同层数使用 `{31,31,31}`，小规模能够恢复记录，但
完整 86 项第一轮点积在切换到单个 31-bit Q 后噪声预算为 0，实际运行失败。

最终适配为 `{31,31,31,31}`：三个数据 Q 加一个特殊 P。两轮点积都执行一次 HPU
`ModSwitch`，递归传输层保留两个 31-bit Q（约 62 bit），而不是原版末层的单个约 36-bit
Q。这样没有删除模数切换、查询展开或第二轮递归；但模数链和分解比率与原版不同：递归层
每个两分量密文分解为 8 个明文。此差异会写入 `pipeline_manifest.json`，不得把该参数称为
原版 bit-for-bit 参数。

在 Ubuntu 软件执行中，小规模两阶段最小噪声预算为 33/33 bit；完整规模第 65535 条记录
测试为 27/28 bit。安全级别仍按项目现有 HPU 参数路径使用 `sec_level_type::none`，这些数值
是正确性与 ST 数据准备结果，不是生产安全参数声明。

## CPU/HPU 边界

CPU：参数和密钥、数据库字节生成、20-bit 拆包、BatchEncode/Decode、客户端加解密、递归
密文分解/重组、最终记录比较。

HPU 图：两个维度的查询展开、全部 Galois 变换、`X^k` 乘法、密文乘明文、密文累加以及
两个回复阶段的模数切换。

跨包数据由 `CompiledApplication::software_result` 从 `BfvSoftwareExecutor` 最终内存重建，
重建结果同时与 SEAL oracle 做完整 RNS word 比较。第二轮不使用 oracle 密文作为输入。

## 构建和测试

```bash
cmake -S . -B build-sealpir \
  -DHPU_ENABLE_SEAL_INTEGRATION=ON \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-sealpir --target \
  hpu_bfv_sealpir_2d_example hpu_validate_package -j2

# 快速语义回归：边界和中间位置
ctest --test-dir build-sealpir \
  -R 'hpu_bfv_sealpir_2d_small_record_' --output-on-failure

# 生成小规模 ST 输入包，并逐包验证
cmake --build build-sealpir --target bfv_sealpir_2d_small_delivery

# 完整 65536 条记录；不会静默缩小，运行和产物体积明显更大
cmake --build build-sealpir --target bfv_sealpir_2d_full_delivery

# 也可直接运行指定位置
./build-sealpir/hpu_bfv_sealpir_2d_example \
  --profile full --record-index 65535
```

完整规模默认按每包一个点积输出分片；可用 `--dot-shard-outputs N` 调整每包输出数，但不会
改变 86×85 布局或运算语义。

## ST 产物和验证边界

输出根目录包含：

- `pipeline_manifest.json`：参数、查询位置、噪声预算、跨阶段来源和所有分片目录；
- `expected_record.bin` / `actual_record.bin`：最终业务 golden 和恢复结果；
- `README.txt`：本次运行的复现与逐包验证命令；
- 每个阶段目录中的 HPU assembly/编码指令、DMA 清单、初始 HPU_MEM 镜像、逐输出 golden、
  计算图以及 oracle/资源报告。

当前结论只包括 SEAL oracle 对拍、HPU 软件执行和应用包验证器通过。没有目标板或 RTL 的
运行日志、IRQ/FAULT 状态和外部 monitor 证据，因此报告固定写为
`Real HPU hardware execution: NOT RUN`，不能称为真实 ST 已通过。
