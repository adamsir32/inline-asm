# Trident PIR/PSI HPU 软件应用交付指南

本工程把固定版本 Trident 的二维递归 PIR 与 APSI PSI 同态计算组织为现有 HPU 编译器可编译的应用图。PIR 查询 65,536 条、每条 288 字节、`N=8192`；PSI 的目标工作量是 1,000,000 个发送方元素和 1,024 个接收方元素。构建、软件执行与实体硬件执行是不同的验收层级。

## 代码和执行边界

| 阶段 | 执行位置 | 实际工作 |
| --- | --- | --- |
| PIR 数据库准备和查询 | CPU | 固定字节生成器、数据库编码、查询加密、阶段间密文分解、最终解密和记录恢复。 |
| PIR 两阶段递归计算 | HPU 应用图 | 查询展开、密文与数据库明文乘加、递归阶段的模数切换。 |
| PSI 集合准备 | CPU | 确定性 OPRF 输出测试夹具、Kuku 位置及接收方布谷鸟表、发送方 BinBundle 字段根多项式、查询源幂加密。 |
| PSI 密文求值 | HPU 应用图 | 缺失幂生成、重线性化、模数切换、普通或 Paterson–Stockmeyer 多项式求值。 |
| PSI 结果恢复 | CPU | 解密每个候选位置的五个字段，按同一 BinBundle 内全为零恢复匹配。 |

Kuku 只用于 PSI 的 CPU 哈希预处理。`HPU_ENABLE_TRIDENT_PSI=OFF` 时，CMake 不查找 Kuku，也不生成 PSI 目标。Trident 的五字段独立根匹配可能产生概率性假阳性；`set_test_data.json` 分别记录原始集合交集、APSI 参考匹配和实际恢复匹配。正式应用只有 Trident 匹配路径；`--match-mode trident` 是兼容旧脚本的可选写法，`exact` 不再接受。

## 构建

以下命令在仓库根目录执行。工程使用仓库自带的 modified-SEAL；首次配置所需的 GSL、zlib、zstd 按其现有 CMake 依赖机制准备。服务器已经缓存这三个依赖的固定源码，最终报告列出实际路径。

```bash
# 不安装 Kuku 时构建 HPU 原有示例与 PIR
cmake -S . -B ../build-no-kuku -DHPU_ENABLE_SEAL_INTEGRATION=ON \
  -DHPU_ENABLE_TRIDENT_PSI=OFF -DCMAKE_DISABLE_FIND_PACKAGE_Kuku=TRUE
cmake --build ../build-no-kuku --target hpu_bfv_evaluator_example \
  hpu_bfv_sealpir_2d_concentrated_example hpu_validate_package -j4

# 在独立前缀中已有 Kuku 2.1 时额外构建 PSI
cmake -S . -B ../build -DHPU_ENABLE_SEAL_INTEGRATION=ON \
  -DHPU_ENABLE_TRIDENT_PSI=ON -DCMAKE_PREFIX_PATH=/path/to/reference-prefix
cmake --build ../build --target hpu_bfv_apsi_concentrated_example \
  hpu_bfv_sealpir_2d_concentrated_example hpu_validate_package -j4
```

如 modified-SEAL 的第三方下载不可达，可给干净构建传入 `FETCHCONTENT_SOURCE_DIR_MSGSL`、`FETCHCONTENT_SOURCE_DIR_ZLIB`、`FETCHCONTENT_SOURCE_DIR_ZSTD` 指向已校验的源码目录；这与 Kuku 开关无关。本次服务器上无 Kuku 的干净构建及有 Kuku 的构建的实测结果见验证报告。

本次服务器缓存可直接复用，完整的无 Kuku 配置命令是：

```bash
AUDIT_ROOT=/home/zhaozhengyi/independent-pir-psi-audit-20261026
cmake -S . -B ../build-no-kuku -DHPU_ENABLE_SEAL_INTEGRATION=ON \
  -DHPU_ENABLE_TRIDENT_PSI=OFF -DCMAKE_DISABLE_FIND_PACKAGE_Kuku=TRUE \
  -DFETCHCONTENT_SOURCE_DIR_MSGSL="$AUDIT_ROOT/deps/GSL-152d6eb989a1ecd23fe9c9cfb2fb8cfc7c0cd0c1" \
  -DFETCHCONTENT_SOURCE_DIR_ZLIB="$AUDIT_ROOT/deps/zlib-da607da739fa6047df13e66a2af6b8bec7c2a498" \
  -DFETCHCONTENT_SOURCE_DIR_ZSTD="$AUDIT_ROOT/deps/zstd-f8745da6ff1ad1e7bab384bd1f9d742439278e99"
```

## 运行和检查交付包

下列脚本运行应用、检查解密结果与独立答案，再逐个调用仓库原有 `hpu_validate_package`。它们会重建指定的输出目录，运行前应选择专用空目录。

```bash
python3 test/sealpir_2d_delivery_test.py \
  --example ../build/hpu_bfv_sealpir_2d_concentrated_example \
  --validator ../build/hpu_validate_package --profile trident \
  --record-index 65535 --output ../delivery/pir-trident

python3 test/apsi_concentrated_delivery_test.py \
  --example ../build/hpu_bfv_apsi_concentrated_example \
  --validator ../build/hpu_validate_package --profile target \
  --scenario multi --output ../delivery/psi-trident

ctest --test-dir ../build --output-on-failure
```

PIR 的 `pipeline_manifest.json` 描述两阶段包、阶段间 CPU 分解及最终记录恢复；`actual_record.bin` 应与 `expected_record.bin` 相同。PSI 的清单描述每个发送方分区对应的包和三种匹配集合。目标规模包可以很大，建议留在服务器证据目录，并用 `sha256sum` 固定清单、最终结果和各包关键文件。

每个包中的计算图记录编译器接收的算子和依赖；汇编和编码指令是同一图的后端产物；DMA 描述数据搬运；`HPU_MEM` 是该软件包的初始化内存内容。上游验证器检查包结构与文件一致性。应用还运行 SEAL 逐节点参考、HPU 软件执行器与主机编码指令模型；这三项通过才支持“软件数值正确”的结论。

## 原生 SenderDB/BinBundle 对照

固定 Trident 提交为 `e3f819f845f3cee7425fa08a3aa64879cfd5b3c0`。在**隔离的参考副本**中，`test/apsi_trident_senderdb_accessor.py` 仅给 `SenderDB` 增加只读 BinBundle 访问器；`test/apsi_trident_senderdb_native.cpp` 调用原始 `SenderDB::insert_or_assign`、Kuku 和真实 BinBundle 缓存。构建该独立驱动后，可用 `--catalog` 生成候选令牌、`test/apsi_trident_senderdb_cases.py` 选取边界样例、`--cross` 生成跨字段接收方，然后运行 `test/apsi_trident_senderdb_differential.py`。

本次服务器上的隔离参考目录是 `$AUDIT_ROOT/trident-faithful-reference`，驱动为其中 `trident-apsi-build/apsi/bin/apsi_native_senderdb`，参数文件为 `trident/apsi/parameters/1M-1024-com.json`。其 CMake 仅在参考副本额外注册该驱动并链接原生 `apsi`，源文件放在参考根目录；只读访问器用 `sender_db.h.unmodified` 生成。原件和访问器差异分别保存在 `sender_db.h.unmodified`、`senderdb-audit-accessor.patch`。新环境应在固定 Trident 提交的**独立复制品**执行这一步，勿修改 HPU 仓库或原作者底层源码。

对新建的隔离参考副本，补充审计入口的最小操作如下。先按 Trident 自身说明完成 APSI 与第三方依赖构建，并把 `CMAKE_PREFIX_PATH` 指向其依赖前缀：

```bash
REF=/path/to/isolated-trident-reference
cp "$REF/trident/apsi/sender/apsi/sender_db.h" \
  "$REF/trident/apsi/sender/apsi/sender_db.h.unmodified"
python3 test/apsi_trident_senderdb_accessor.py \
  --source "$REF/trident/apsi/sender/apsi/sender_db.h.unmodified" \
  --output "$REF/trident/apsi/sender/apsi/sender_db.h"
cp test/apsi_trident_senderdb_native.cpp "$REF/apsi_trident_senderdb_native.cpp"
cat >> "$REF/trident/apsi/CMakeLists.txt" <<'CMAKE'
add_executable(apsi_native_senderdb ${CMAKE_CURRENT_LIST_DIR}/../../apsi_trident_senderdb_native.cpp)
target_link_libraries(apsi_native_senderdb PRIVATE apsi)
target_link_directories(apsi_native_senderdb PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../reference-prefix/lib)
CMAKE
cmake --build "$REF/trident-apsi-build" --target apsi_native_senderdb -j4
```

在上述服务器参考驱动已经构建后，可重复生成输入并运行全部比较：

```bash
REF="$AUDIT_ROOT/trident-faithful-reference"
NATIVE="$REF/trident-apsi-build/apsi/bin/apsi_native_senderdb"
PARAMS="$REF/trident/apsi/parameters/1M-1024-com.json"
"$NATIVE" --catalog "$PARAMS" 100000 "$REF/senderdb-catalog.txt"
python3 test/apsi_trident_senderdb_cases.py \
  --catalog "$REF/senderdb-catalog.txt" --output-dir "$REF/senderdb-cases"
"$NATIVE" --cross "$PARAMS" "$REF/senderdb-cases/shared_bin.raw" \
  "$REF/senderdb-cases/cross_felt.raw"
python3 test/apsi_trident_senderdb_differential.py \
  --native "$NATIVE" --params "$PARAMS" \
  --example ../build/hpu_bfv_apsi_concentrated_example \
  --case-dir "$REF/senderdb-cases" --output "$REF/senderdb-final-differential" \
  --full single shared_bin partial_felt ps_degree_30 extra_bundle_125 cross_felt
```

差分脚本逐项比较令牌字段、Kuku 候选及接收方摆放、每个 BinBundle 的每槽根和多项式系数、幂次 DAG、原生判零结果。BinBundle 实例顺序允许不同，因此按完整槽内容的多重集合比较；HPU 批处理多出的一个槽必须为空槽多项式 1。`--full` 指定样例还会检查最终 SEAL、HPU 软件和指令模型结果。实际运行命令、结果和日志路径见验证报告。

## 当前能力边界

- 没有执行实体 HPU 或 RTL；软件时间不可当作硬件性能。
- 没有实现 FourQ OPRF 网络协议。普通应用用确定性输出夹具；原生 SenderDB 审计用固定合法 OPRF 标量生成发送方真实令牌，跨字段接收方在令牌边界注入合成反例。
- PIR 保留 HPU 所需的 31 位系数模数链适配。阶段间密文使用经过 SEAL、HPU 软件及指令模型验证的参考密文代理，因此最终记录对照不等于两种模数链的中间密文完全相同。
- APSI 的概率性匹配结果不能强制解释为原始整数集合的精确交集。

PIR 数据库每个字节由 `test/pir_record_oracle.py` 中的 `splitmix64(seed ^ (index << 32) ^ (offset // 8))` 的对应字节确定；默认种子为 `0x5345414C50495232`。因此可以对任一索引独立重建 288 字节答案，不依赖应用的内部数据库对象。
