# Microsoft SealPIR 二维 BFV HPU 应用

实现入口：`examples/bfv_sealpir_2d_application.cpp`

- `hpu_bfv_sealpir_2d_concentrated_example`：每轮递归编成一张图，共两个应用包。

本程序按照 Microsoft SealPIR 的 `src/main.cpp`、`src/pir.cpp`、
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
3. `stage0_concentrated_000` 将 SealPIR 的 Galois 元素在应用内换算为 BFV 行旋转步数，
   用原有 HPU `rotate_rows`、`Add`、乘 `X^k`、`Negate`、`MultiplyPlain` 和 `Add`
   完成第一维查询展开与全部点积，并执行一次 HPU `ModSwitch`。
4. 公共 API 不暴露 HPU 软件执行器内存。每包只有在 `compiled.verified()` 和
   `compiled.instruction_verified()` 都通过后，CPU 才使用逐 RNS 系数验证一致的 SEAL
   oracle 代理密文，把第一轮结果按 20-bit 块分解为 BFV 明文。
5. `stage1_concentrated_000` 以第一轮分解明文为数据库，展开第二维加密查询并执行第二轮
   HPU 点积和 `ModSwitch`，得到递归回复。
6. 客户端解密回复明文，每 8 个明文重组为一个两分量密文，再次解密、BatchDecode，恢复
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

跨包数据使用原始 `CompiledApplication::oracle_result`；调用前强制检查功能软件执行和编码
指令模型两项验证状态。因功能验证已经把 HPU 软件执行的 RNS words 与 oracle 逐系数比较，
该密文是验证通过后的阶段边界代理。它不是从真实 HPU/RTL 读回的密文，不能据此声称硬件
ST 已通过。

## 构建和测试

```bash
cmake -S . -B build-sealpir \
  -DHPU_ENABLE_SEAL_INTEGRATION=ON \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build-sealpir --target \
  hpu_bfv_sealpir_2d_concentrated_example hpu_validate_package -j2

# 快速语义回归：边界和中间位置
ctest --test-dir build-sealpir \
  -R 'hpu_bfv_sealpir_2d_concentrated_small_record_' --output-on-failure

# 生成小规模 ST 输入包，并逐包验证
cmake --build build-sealpir --target bfv_sealpir_2d_concentrated_small_delivery

# 完整 65536 条记录；不会静默缩小，运行和产物体积明显更大
cmake --build build-sealpir --target bfv_sealpir_2d_concentrated_full_delivery

# 也可直接运行指定位置
./build-sealpir/hpu_bfv_sealpir_2d_concentrated_example \
  --profile full --record-index 65535
```

## 集中版：两张大图和统一执行入口

集中版不是用一个输出密文生成一个包，而是在同一个 `ApplicationBuilder` 中记录本轮的
压缩查询导入、完整 `expand_query`、所有数据库 `MultiplyPlain`/`Add` 归约以及输出
`ModSwitch`。默认第一轮的 85 个密文输出在一个包，第二轮的 8 个递归回复在另一个包，
所以完整查询固定生成 **2 个 HPU 应用包**。

当前不能把两轮合成单包。原因是 SealPIR 算法在轮间要求 CPU 把第一轮密文的两个 RNS
分量拆为 20-bit 明文块；第二轮数据库只有第一包的软件验证通过后
才能确定。
现有 HPU 图没有“密文 RNS → 20-bit BFV 明文块”的动态分解算子，也不能在静态
`ApplicationBuilder` 图中回调 CPU。集中版因此采用可达到的最小边界：

1. 执行 `stage0_concentrated_000`；
2. 检查第一阶段功能软件执行与指令模型验证，并读取逐系数一致的 oracle 代理密文，在 CPU 分解；
3. 以这些验证后的分解结果构造并执行 `stage1_concentrated_000`；
4. CPU 重组、解密和恢复记录。

这个完整顺序由一个可执行文件自动连续执行；`pipeline_manifest.json` 的
`execution_pipeline` 同时记录包间依赖。这里使用 oracle 代理是原始公共 API 的限制，且只有
在 HPU 软件结果逐系数验证通过后才允许；没有跳过第一阶段计算图或软件执行。

集中版默认关闭资源重用优化器。完整 85 输出大图开启优化后，在测试服务器上超过 15 分钟
仍未完成；关闭优化并不会删除运算，只是不对临时 HPU 地址做生命周期复用。结果是可在约
3 分钟内稳定完成编译和逐指令软件回放，但单包 HPU 地址空间和主机峰值内存明显增加。
可显式传入 `--optimize-resources` 重试优化路径，不能把默认编译耗时下降理解为 HPU
执行性能提高。

```bash
cmake --build build-sealpir --target \
  hpu_bfv_sealpir_2d_concentrated_example hpu_validate_package -j2

# 小规模三位置回归
ctest --test-dir build-sealpir \
  -R 'hpu_bfv_sealpir_2d_concentrated_small_record_' --output-on-failure

# 统一运行完整 86x85 查询（两包，自动执行轮间 CPU 分解）
./build-sealpir/hpu_bfv_sealpir_2d_concentrated_example \
  --profile full --record-index 65535

# 生成并校验 ST 交付目录
cmake --build build-sealpir --target bfv_sealpir_2d_concentrated_small_delivery
cmake --build build-sealpir --target bfv_sealpir_2d_concentrated_full_delivery
```

## 完整规模实测

以下数据来自同一台 Ubuntu 服务器、Release 构建、记录 65535、完整 65536×1024-byte
数据库和 86×85 布局。时间包括图记录、编译、SEAL oracle、HPU 软件执行器和最终恢复，
不是实体 HPU 执行时间。

| 指标 | 两包集中版 |
|---|---:|
| 应用包 | 2 |
| 编码指令 | 4,902,334 |
| DMA span | 1,733,709 |
| 最大单包 HPU_MEM 行 | 7,266,011 |
| stage0/stage1 最小噪声 | 27/28 bit |
| 最终 1024-byte 记录 | PASS |

集中版默认关闭地址复用优化器；这不会删除运算，但会增加单包地址空间和编译主机内存。
包数减少只简化 ST 调度与依赖，不能外推为实体 HPU 性能提升。实际硬件吞吐、带宽和周期
必须以后续板卡测量为准。

## ST 产物和验证边界

输出根目录包含：

- `pipeline_manifest.json`：参数、查询位置、噪声预算、跨阶段来源和两个阶段目录；
- `expected_record.bin` / `actual_record.bin`：最终业务 golden 和恢复结果；
- `README.txt`：本次运行的复现与逐包验证命令；
- 每个阶段目录中的 HPU assembly/编码指令、DMA 清单、初始 HPU_MEM 镜像、逐输出 golden、
  计算图以及 oracle/资源报告。

当前结论只包括 SEAL oracle 对拍、HPU 软件执行和应用包验证器通过。没有目标板或 RTL 的
运行日志、IRQ/FAULT 状态和外部 monitor 证据，因此报告固定写为
`Real HPU hardware execution: NOT RUN`，不能称为真实 ST 已通过。

本次服务器实际生成的集中版目录为：

- `outputs/bfv_sealpir_2d_concentrated_small`：约 52 MiB、808 个文件、2 个包；
- `outputs/bfv_sealpir_2d_concentrated_full`：约 5.7 GiB、102,328 个文件、2 个包。

同步原作者上游提交 `7306d1c` 后，完整目录生成并逐包验证耗时 6:07.71，验证脚本峰值
RSS 为 31,080,760 KiB；两个目录的
`stage0_concentrated_000` 和 `stage1_concentrated_000` 均已由
`hpu_validate_package` 校验通过。这是可交给后续 ST 的输入，不代表目标板 ST 已通过。
同步前后的完整对比及测试环境边界见
[PIR/PSI 上游同步与验证审计](PIR_PSI_UPSTREAM_SYNC_AUDIT.md)。
