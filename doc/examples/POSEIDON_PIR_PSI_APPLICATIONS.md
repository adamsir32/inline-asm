# Poseidon PIR/PSI 的 HPU 应用移植

本目录下的两个程序把 Poseidon 所引用的 PIR/PSI 同态计算核心接入
`ApplicationBuilder` / `Evaluator`，并像 `CKKS x²+1` 示例一样完成：输入与密钥准备、
HPU 图构建、SEAL oracle、HPU 软件执行器逐系数比较、最终指令 host 回放、业务语义
检查和应用包输出。

这两个程序是**单进程、同态核心层的完整可运行 demo**，不是网络服务。APSI 的 OPRF、
Cuckoo Hash、传输协议和标签处理，以及 SealPIR 的网络序列化，仍属于 host/protocol 层，
不在 HPU 指令翻译范围内。

## 证据来源

- Poseidon PIR 文档明确采用 BFV，并把服务端核心命名为 `expand_query` 和
  `generate_reply`：<https://poseidon-hpu.readthedocs.io/en/latest/Benchmark/Application/pir_bfv/>
- Poseidon PSI 文档明确采用 APSI 的 OPRF、Cuckoo Hash、查询幂和低深度多项式求值：
  <https://poseidon-hpu.readthedocs.io/en/latest/Benchmark/Application/PSI/>
- Poseidon 历史提交 `e3bc579` 的 HEDAG 产物记录了未公开提交的原始路径
  `Trident/pir_bfv/pir_server.cpp` 和 `Trident/apsi/...`，以及
  `apply_galois`、`multiply_plain`、`add`、`square/multiply`、`relinearize`、
  `drop_modulus` 等调用。
- PIR 的可恢复实现与小测试来自 SealPIR：
  <https://github.com/microsoft/SealPIR>
- PSI 的参数、测试规模、`PowersDag` 和 `BatchedPlaintextPolyn::eval` 来自 APSI：
  <https://github.com/microsoft/APSI>

由于 Poseidon 公共仓库没有提交完整 `Trident` 源码，下面把“原样保留”和“移植适配”
逐项分开；未把推断伪装成 Poseidon 原始行为。

## BFV PIR

入口：`examples/bfv_poseidon_pir_application.cpp`

该文件继续保留原先的一维实验，不因新增工作而删除。严格采用 BatchEncoder、两轮递归、
4×4/86×85 两种规模的独立实现见
[`SEALPIR_2D_HPU_APPLICATION.md`](SEALPIR_2D_HPU_APPLICATION.md)。

原样保留的规模和流程：

| 项目 | 值 |
|---|---:|
| 数据库记录数 | 2048 |
| 每条记录 | 288 bytes |
| BFV `N` | 4096 |
| 每个明文的可用位数 | 20 |
| PIR 维度 `d` | 1 |
| 每明文记录数 | 35 |
| 实际数据库明文数 | 59 |
| 一维补齐后的查询长度 | 60 |

查询展开逐层执行 SealPIR 的通用 `ApplyGalois`、加法、乘 `X^k` 和加法；随后对 60 个
展开密文执行数据库 `MultiplyPlain` 与归约 `Add`。解密后按 SealPIR 的 20-bit 字节拆分
规则恢复完整 288 字节记录，而不只检查一个数值。

这里必须区分“字节拆分规则”和“BFV 明文布局”：SealPIR 原版先把字节拆成 20-bit 值，
再用 `BatchEncoder::encode` 把这些值变为 BFV 明文多项式；客户端解密后也先
`BatchEncoder::decode`。当前 demo 为了让查询展开选中的多项式系数直接对应记录块，省略了
这层 BatchEncoder 置换并直接写入多项式系数。因此压缩查询展开、同态点积和完整记录恢复
已经运行验证，但数据库布局不是 SealPIR 的严格一比一复刻；这是后续提高保真度的明确任务。

显式适配：

1. `BFVDefault(4096)` 的 Q/P 超过当前 HPU 的 31 位模数 ABI，替换为四枚 30 位素数。
2. 测试原来随机选择记录；应用包为了可复现固定选择索引 1234。
3. SealPIR 把 59 个数据库明文补齐到 60；当前 modified-SEAL 开启透明密文异常，因此
   不可达的第 60 个 padding 明文放置非零哨兵。
4. `-X^k` 被等价展开为 `MultiplyPlain(X^k)` 后接 `Negate`，以适配 HPU 的正单项式
   prepared-plaintext 路径。
5. 数据库和回复采用直接多项式系数布局，尚未接入 SealPIR 的
   `BatchEncoder::encode/decode` 布局变换。

## BFV PSI

入口：`examples/bfv_poseidon_psi_application.cpp`

采用 APSI `create_params1()` 和 `UnlabeledSmall1` 中的精确应用规模：

| 项目 | 值 |
|---|---:|
| sender items | 10 |
| receiver items | 5 |
| intersection | 2 |
| `felts_per_item` | 8 |
| hash functions | 3 |
| `max_items_per_bin` | 16 |
| table size | 4096 |
| source query powers | `{1,3,5}` |
| BFV `N` | 8192 |
| APSI plaintext modulus | 65537 |

程序固定使用交集 `{1,4}`，构造 sender root polynomial，并在五个 receiver 槽上完成
零测试；前两个槽必须为 0，其余三个槽必须与明文有限域求值完全一致。

这里的 HPU BFV 路径存在已实测的兼容边界，因此有以下显式适配：

1. 跳过 OPRF/Cuckoo 的 host 实现，直接使用确定性的有限域元素；表大小、哈希数等保留为
   来源元数据，而不是声称在 HPU 图内执行。
2. `BFVDefault(8192)` 替换为四枚 31 位 Q/P；APSI 的 `t=65537` 替换为本项目
   `PlainModulus::Batching(8192,17)` 得到的 `t=114689`。集合零测试语义不变。
3. 原 APSI 客户端只发送 `{x,x³,x⁵}`，服务端生成至 `x¹⁶`。当前 HPU 模型在此参数下的
   深幂 Relinearize/ModSwitch 无法通过 SEAL 逐系数差分，所以 demo 由客户端发送实际求值
   所需的 `x¹..x¹⁰`，并剪掉不会被 degree-10 多项式消费的 `x¹¹..x¹⁶`。
4. APSI 的 power/result ModSwitch 暂不进入该 HPU 图；NTT 往返由 HPU 算子内部管理。
5. 大系数 `MultiplyPlain(C,a)` 在该参数下也不能通过现有 prepared-plaintext 差分，故等价
   下沉为二进制 double-and-add 的密文 `Add` 图；常数项仍使用 `AddPlain`。

这些限制都打印在程序输出中。它们说明 PSI demo 已完整验证 degree-10 零测试内核，但还不
应称为 Poseidon/APSI 协议或原始算子图的一比一替代。

## 构建、运行和应用包验证

```bash
cmake -S . -B build-poseidon -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DHPU_ENABLE_SEAL_INTEGRATION=ON \
  -DBUILD_TESTING=ON

cmake --build build-poseidon --target \
  hpu_bfv_poseidon_pir_example \
  hpu_bfv_poseidon_psi_example \
  hpu_validate_package

./build-poseidon/hpu_bfv_poseidon_pir_example \
  --emit-dir outputs/bfv_poseidon_pir_application
./build-poseidon/hpu_bfv_poseidon_psi_example \
  --emit-dir outputs/bfv_poseidon_psi_application

./build-poseidon/hpu_validate_package outputs/bfv_poseidon_pir_application
./build-poseidon/hpu_validate_package outputs/bfv_poseidon_psi_application
```

两个可执行文件都要求固定的来源规模；传入其他 `--degree` 会直接报错，而不会悄悄改变
实验规模。生成包包含 assembly、编码指令、DDR/HPU_MEM 初始镜像、golden、操作图、
语义报告和资源/验证报告。
