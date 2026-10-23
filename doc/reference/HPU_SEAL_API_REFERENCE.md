# HPU 应用编程手册

本手册面向使用当前项目编写 CKKS、BFV、BGV 应用和测试用例的程序员。
统一入口为 `hpu/seal/evaluator.hpp`，命名空间为 `hpu::seal_adapter`。

阅读顺序：先运行第 2 节的完整程序，再按第 5～7 节查询接口与方案约束。
只想运行现成案例可阅读 [Evaluator 前端入门](../getting-started/HPU_EVALUATOR_FRONTEND.md)。
显式 Plan、镜像构建器、软件执行器和算子生成 API 见
[底层 API 参考](HPU_LOW_LEVEL_API_REFERENCE.md)；分层设计见
[项目架构](../architecture/HPU_ARCHITECTURE.md)。

## 目录

- [1. 编程流程与职责](#1-编程流程与职责)
- [2. 完整示例：CKKS 计算 x²+1](#2-完整示例ckks-计算-x²1)
- [3. 编译、运行与重复生成](#3-编译运行与重复生成)
- [4. 句柄、计算图与命名](#4-句柄计算图与命名)
- [5. 应用构建与编译结果接口](#5-应用构建与编译结果接口)
- [6. Evaluator 接口与支持范围](#6-evaluator-接口与支持范围)
- [7. 三方案的编程规则](#7-三方案的编程规则)
- [8. 编译选项与诊断](#8-编译选项与诊断)
- [9. golden、数学验证与交付](#9-golden数学验证与交付)
- [10. 常见错误与排查](#10-常见错误与排查)
- [11. 完整案例与进阶入口](#11-完整案例与进阶入口)

## 1. 编程流程与职责

一个应用生成程序通常完成以下步骤：

1. 用 SEAL 创建上下文、密钥，编码并加密输入。
2. 用 `ApplicationBuilder` 导入密文和明文。
3. 用 HPU `Evaluator` 记录计算图，显式添加重线性化、旋转和降层等操作。
4. 用 `mark_output` 命名要交付的结果。
5. 用 `compile` 自动生成 SEAL oracle，完成软件对拍和资源编译。
6. 检查解密后的数学结果，用 `write` 或公共包接口导出到 `outputs/`。

| 应用代码负责 | 框架负责 |
| --- | --- |
| 选择方案、参数、输入和数学公式 | 根据 context 选择方案实现，检查句柄与元数据 |
| 用 SEAL 编码、加密和生成评估密钥 | 转换 HPU 数据布局，准备常量、twiddle、密钥镜像与工作区 |
| 显式选择重线性化、降层和 scale 调整 | 构造资源 IR，分配 DDR、对象槽位与 bank，生成指令和 DMA |
| 编写数学预期和解码容差 | 自动比较 SEAL、方案 SoftwareExecutor 与最终指令 host 模型 |
| 指定导出路径并归档 | 写出指令、内存、golden、图元数据与验证报告 |

HPU `Evaluator` 的调用记录图，`compile()` 执行 host 验证与编译；目标 HPU
执行由 IT/Nexus-AM 接入。C++ 中的普通循环和条件判断在构图时执行，当前生成
静态图与静态指令序列。

当前底层使用 [HPU ISA v1.2](HPU_PROGRAMMING_MANUAL.md)。本轮 DMA MOD/REL/CP
更新不改变本手册的顶层 API。编译器根据最终内存分配的 `kind=twiddle`，自动为
对应 DLOAD 设置 CP=1，覆盖 stage 参数和 pre/post 向量；其他 DLOAD 及全部
DSTORE 设置 CP=0。该策略也覆盖编译器插入的重载，编写应用无需手工设置 CP。
已有应用重新编译并导出完整包即可，旧 `.inst32/.cmd26` 与 ELF 不能复用。
底层手工 CP 接口和 LLC 提示的限制见
[DMA 参数与 LLC 提示](HPU_LOW_LEVEL_API_REFERENCE.md#11-dma-参数与可选-llc-分配提示)。

## 2. 完整示例：CKKS 计算 x²+1

将下面代码保存为 `examples/my_ckks_application.cpp`。程序使用 N=128 的功能
演示参数，计算 `y=x²+1`，检查解码误差，并导出带语义报告的交付包。
部署规模的参数选择参考现有 N=65536 前端示例。

```cpp
#include "hpu/seal/application_delivery.hpp"
#include "hpu/seal/ckks_context.hpp"
#include "hpu/seal/evaluator.hpp"
#include <seal/seal.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    try {
        std::filesystem::path emit_directory = "outputs/my_ckks_application";
        if (argc == 3 && std::string(argv[1]) == "--emit-dir") {
            emit_directory = argv[2];
        } else if (argc != 1) {
            throw std::invalid_argument("usage: hpu_my_ckks_application [--emit-dir PATH]");
        }

        // 1. host SEAL：上下文、密钥、编码与加密。
        hpu::seal_adapter::CkksContextSpec spec;
        spec.poly_modulus_degree = 128;
        spec.coeff_modulus_bits = {20, 20, 20, 20};
        const auto bundle = hpu::seal_adapter::create_ckks_context(spec);
        const auto& context = *bundle.context;
        seal::KeyGenerator keygen(context);
        seal::RelinKeys relin_keys;
        keygen.create_relin_keys(relin_keys);

        const std::vector<double> input{0.25, -1.5, 2.0, 0.75};
        seal::CKKSEncoder encoder(context);
        seal::Plaintext encoded_input;
        encoder.encode(input, std::ldexp(1.0, 20), encoded_input);
        seal::Ciphertext encrypted_input;
        seal::Encryptor(context, keygen.secret_key())
            .encrypt_symmetric(encoded_input, encrypted_input);

        // 2. HPU 前端：记录图，并显式进行乘法后处理。
        hpu::seal_adapter::ApplicationBuilder app(context);
        auto evaluator = app.evaluator();
        const auto x = app.import_ciphertext("input/x", encrypted_input);
        hpu::seal_adapter::Ciphertext y;
        evaluator.square(x, y);                         // 2 → 3 分量
        evaluator.relinearize_inplace(y, relin_keys);   // 3 → 2 分量
        evaluator.rescale_to_next_inplace(y);          // 降一层并更新 scale

        // 偏置按消费它的密文 level 和 scale 编码。
        seal::Plaintext encoded_bias;
        encoder.encode(std::vector<double>(input.size(), 1.0),
                       y.parms_id(), y.scale(), encoded_bias);
        const auto bias = app.import_plaintext("constant/bias", encoded_bias);
        evaluator.add_plain_inplace(y, bias);
        app.mark_output("y", y);

        // 3. 自动执行 SEAL、方案软件模型和最终指令模型对拍。
        const auto compiled = app.compile("my_ckks_application");

        // 4. 检查所构建的图是否表达了预期公式。
        seal::Plaintext decrypted;
        seal::Decryptor(context, keygen.secret_key())
            .decrypt(compiled.oracle_result(y), decrypted);
        std::vector<double> decoded;
        encoder.decode(decrypted, decoded);
        double maximum_error = 0.0;
        for (std::size_t i = 0; i < input.size(); ++i) {
            const double expected = input[i] * input[i] + 1.0;
            if (!std::isfinite(decoded.at(i))) {
                throw std::runtime_error("decoded result is not finite");
            }
            maximum_error = std::max(maximum_error, std::abs(decoded[i] - expected));
        }
        if (maximum_error >= 1e-2) {
            throw std::runtime_error("CKKS decoded error exceeded tolerance");
        }

        // 5. 给包附加数学验证报告，然后一次导出。
        auto request = compiled.package();
        request.semantic_report = hpu::seal_adapter::ckks_delivery_semantics(
            decoded, input.size(), maximum_error, 1e-2);
        hpu::delivery::write_application_package(emit_directory, request);
        std::cout << "Package: " << emit_directory << '\n'
                  << "Maximum decoded error: " << maximum_error << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
```

此例的期望结果为 `[1.0625, 3.25, 5.0, 1.5625]`。仅检查输入使用的前四个槽位。
`oracle_result(y)` 是 SEAL 参考结果，实际 HPU 读回由 IT 完成。
若不需要附加 `semantic/decoded.json`，导出部分可直接使用：

```cpp
compiled.write(emit_directory);
```

两种方式选择一种导出，目标目录要求不存在。

## 3. 编译、运行与重复生成

### 3.1 为自己的 cpp 添加构建目标

在仓库 `CMakeLists.txt` 已有 SEAL 集成目标定义之后添加：

```cmake
if(HPU_ENABLE_SEAL_INTEGRATION)
    add_executable(hpu_my_ckks_application examples/my_ckks_application.cpp)
    target_link_libraries(hpu_my_ckks_application PRIVATE hpu_seal_delivery)
endif()
```

链接 `hpu_seal_delivery` 会传递所需的头文件与库依赖。方案均使用这个目标；
不要另外混用另一份 SEAL 安装。当前包写入实现使用 POSIX 文件接口，生成程序
在 Linux host 上运行。

从仓库根目录执行：

```bash
cmake -S . -B build -DHPU_ENABLE_SEAL_INTEGRATION=ON
cmake --build build -j2 --target hpu_my_ckks_application hpu_validate_package
./build/hpu_my_ckks_application --emit-dir outputs/my_ckks_application
./build/hpu_validate_package outputs/my_ckks_application
```

`hpu_validate_package` 检查磁盘包的结构、清单、布局和文件完整性，不会重新执行
SEAL，也不运行 HPU。

### 3.2 先运行仓库现成示例

```bash
cmake -S . -B build -DHPU_ENABLE_SEAL_INTEGRATION=ON
cmake --build build -j2 --target \
  hpu_ckks_evaluator_example hpu_bfv_evaluator_example hpu_bgv_evaluator_example

# 统一生成三个方案的小规模前端应用包。
cmake --build build --target hpu_frontend_delivery

# 部署规模 N=65536，输出目录带 _n65536 后缀。
cmake --build build -j2 --target hpu_frontend_deployment_delivery
```

现有前端示例支持 `--degree N`、`--emit-dir PATH`、`--print-asm`、`--no-opt` 和
指令模型诊断选项。第 2 节的独立程序只实现了 `--emit-dir`；CLI 由用例自己解析，
并非 `ApplicationBuilder` 自动提供。

### 3.3 重复生成到同一目录

直接 `write()` 或执行第 2 节程序要求目标目录不存在。可指定新目录，或通过
公共生成脚本重复发布已验证的同案例包：

```bash
python3 tools/generate_application_package.py \
  --example ./build/hpu_my_ckks_application \
  --validator ./build/hpu_validate_package \
  --output outputs/my_ckks_application
```

脚本先在临时目录生成和校验，再替换同方案、同案例的有效包。普通目录、损坏包
或其他案例不会被覆盖。内置 delivery 构建目标也使用这条流程。

## 4. 句柄、计算图与命名

### 4.1 两类 Ciphertext / Plaintext

| 类型 | 用途 |
| --- | --- |
| `seal::Ciphertext / seal::Plaintext` | host 上真实的 SEAL 数据，用于编码、加密、导入和解密 |
| `hpu::seal_adapter::Ciphertext / Plaintext` | 图中逻辑值的句柄，用作 HPU Evaluator 的输入和输出 |

通过 `import_ciphertext`、`import_plaintext` 从 SEAL 数据取得 HPU 句柄。
空的 `Ciphertext` 可作为输出参数；作为输入或查询元数据前必须已被赋值。
所有输入句柄必须属于当前 `ApplicationBuilder`，相同 context 的两个 builder
也不能直接混用句柄。

### 4.2 复制与 inplace

句柄代表不可变逻辑值。复制句柄保留原值，`_inplace` 创建新值并更新传入句柄：

```cpp
auto original = x;
auto y = x;
evaluator.add_inplace(y, x);
// original 和 x 仍表示原输入；y 表示 x+x。
```

图上的 inplace 不承诺覆盖同一个 DDR 地址。物理存储由编译器分配。
输入在导入时捕获快照，评估密钥在操作记录时捕获快照；后续修改原 SEAL 对象
不会改变已记录值，继续构图也不会改变已经编译的包。

### 4.3 分支与多个输出

以已有 CKKS `x` 和对应 Galois key 为例：

```cpp
hpu::seal_adapter::Ciphertext rotated, conjugated, mixed;
evaluator.rotate_vector(x, 1, galois_keys, rotated);
evaluator.complex_conjugate(x, galois_keys, conjugated);
evaluator.add(rotated, conjugated, mixed);
app.mark_output("rotated", rotated);
app.mark_output("mixed", mixed);
```

两个旋转节点都引用 `x`，`mixed` 引用两个分支，依赖由句柄确定。
部署只保留全部 `mark_output()` 指定的结果；未标记的内部结果由编译器管理，
不强制写回 DDR。未连接到输出的计算可被删除。没有标记时导出最后一个计算结果，
而不是最后导入的输入。显式标记中间值即可让 IT 额外读回；它会限制相关写回优化。
多个标签或 scale view 可以引用同一物理结果，包内 golden 不重复。

### 4.4 名称规则

导入名称和输出标签由 `/` 分隔标识符段：每段以英文字母或 `_` 开始，其后使用英文字母、
数字或 `_`。例如 `input/x`、`constant/bias`、`result/y`。空段、尾部 `/`、空格、
`-` 和以数字开头的段均不允许。精确名称 `output` 和前缀 `frontend/` 被保留。

密文与明文的导入名称在同一 builder 内共同唯一；输出标签在输出列表内唯一。
`compile(stem)` 使用简单标识符，例如 `my_ckks_application`；导出目录可以另外指定。

## 5. 应用构建与编译结果接口

### 5.1 ApplicationBuilder

构造方式为 `ApplicationBuilder(const seal::SEALContext& context)`。
context 必须是有效的 CKKS/BFV/BGV，N 为 `[128,65536]` 内的 2 次幂，
参数还需满足对应方案和算子的 HPU 约束，见第 7 节。

| 方法 | 用法与要求 |
| --- | --- |
| `import_ciphertext(id, input)` | 导入有效的 2 或 3 分量 SEAL 密文；BFV 必须是系数域，CKKS/BGV 必须是 NTT 域 |
| `import_plaintext(id, input)` | 导入有效的 SEAL 明文；CKKS 必须是 NTT 域，BFV/BGV 使用系数域明文 |
| `evaluator()` | 返回与此 builder 共享图状态的 `Evaluator` |
| `mark_output(id, value)` | 标记当前应用中的计算结果，原始输入及仅由输入修改 scale 得到的视图不能标为结果 |
| `compile(stem, options={})` | 编译快照并返回 `CompiledApplication`，要求图至少包含一个计算操作 |
| `context()` | 查询应用使用的 SEAL context |

不显式标记输出时，编译器使用最后一个计算结果；该结果的最新 scale view 可提供
导出元数据。后续导入输入或修改其他旧值的 scale 不会改变隐式结果。
`ApplicationBuilder` 不可复制；每个应用建立自己的 builder。

### 5.2 值句柄

| 查询方法 | Ciphertext | Plaintext | 含义 |
| --- | --- | --- | --- |
| `valid()` | 支持 | 支持 | 是否为有效句柄；可用于检查空输出参数 |
| `value_id()` | 支持 | 支持 | 逻辑值名称，用于日志和图定位 |
| `parms_id()` | 支持 | 支持 | SEAL 参数层级标识 |
| `scale()` | 支持 | 支持 | scale，主要用于 CKKS |
| `size()` | 支持 | 无 | 密文分量数，常见为 2 或 3 |
| `correction_factor()` | 支持 | 无 | BGV correction factor |

除了 `valid()`，对空句柄的查询会抛出异常。元数据在构图时推导，因此可在
`compile()` 前读取 `y.parms_id()`、`y.scale()` 来编码下一步使用的明文。
BFV/BGV 导入的系数域明文通常使用零 `parms_id`，其实际 HPU 表示由消费者的层级
和操作决定。

### 5.3 CompiledApplication

| 方法 | 返回内容或含义 |
| --- | --- |
| `write(directory)` | 写出包，返回 `ApplicationPackageReport`；目录必须不存在 |
| `package()` | 返回包请求的只读引用；需要附加 semantic 时可复制请求后交给公共 writer |
| `oracle_result(value)` | 返回该编译快照中对应值的 SEAL 密文；值必须来自同一 builder 且已包含在快照中 |
| `node_count()` | 实际计算节点数，不包含导入和 `set_scale` 元数据视图 |
| `verified()` | SEAL 与方案 SoftwareExecutor 的功能比较通过 |
| `instruction_verified()` | 最终指令 host 模型与 golden 的有效 word 比较通过 |
| `resource_report_json()` | DDR、DMA、对象槽位、bank、spill/reload 等统计 |

`oracle_result` 可用于 host 解密验证。`package()` 的请求包含编译结果所拥有的
镜像指针，复制请求后应保持 `CompiledApplication` 存活直至导出完成。

## 6. Evaluator 接口与支持范围

### 6.1 支持矩阵

| 操作 | CKKS | BFV | BGV |
| --- | --- | --- | --- |
| `add` / `sub` / `multiply` / `square` | 支持 | 支持 | 支持 |
| `add_plain` / `sub_plain` / `multiply_plain` | 支持 | 支持 | 支持 |
| `relinearize`：3 → 2 分量 | 支持 | 支持 | 支持 |
| `negate` | 支持 | 支持 | 前端暂不支持 |
| `rescale_to_next` | 支持 2、3 分量 | 不适用 | 不适用 |
| `mod_switch_to_next` | 前端暂不支持 | 支持 | 支持 |
| `rotate_vector` / `complex_conjugate` | 支持 | 不适用 | 不适用 |
| `rotate_rows` / `rotate_columns` | 不适用 | 支持 | 支持 |
| `set_scale` | 支持 | 不适用 | 不适用 |

该表描述统一前端的范围；底层存在某个算子不代表前端已有对应能力。

### 6.2 算术方法

下表中的 `a`、`b`、`out` 为 HPU `Ciphertext`，`p` 为 HPU `Plaintext`：

| 调用 | 输入要求 | 输出 |
| --- | --- | --- |
| `add(a, b, out)` | 两个 2 分量密文，同 `parms_id`；CKKS scale 兼容 | 2 分量，同层级；BGV 自动按规则平衡 correction factor |
| `sub(a, b, out)` | 与 add 相同 | 2 分量，表示 a−b |
| `multiply(a, b, out)` | 两个 2 分量密文，同 `parms_id`；BFV 当前 Q 至少有两个模数 | 3 分量；CKKS scale 相乘，BGV factor 相乘 mod t |
| `square(a, out)` | 2 分量；BFV 当前 Q 至少有两个模数 | 与 multiply(a,a) 相同 |
| `negate(a, out)` | 2 分量，CKKS/BFV | 2 分量，同层级 |
| `add_plain(a, p, out)` | a 为 2 分量；CKKS 明文与密文同 level、scale 兼容 | 2 分量，同层级 |
| `sub_plain(a, p, out)` | 与 add_plain 相同 | 2 分量，表示 a−p |
| `multiply_plain(a, p, out)` | a 为 2 分量；CKKS 明文与密文同 level | 2 分量；CKKS scale 乘以明文 scale |

三分量乘法结果需先重线性化，才能继续使用当前要求二分量的加法、乘法、明文
运算或旋转。CKKS rescale 可在重线性化之前或之后使用，支持 2、3 分量。

### 6.3 重线性化、降层与 scale

| 调用 | 要求与行为 |
| --- | --- |
| `relinearize(a, relin_keys, out)` | a 必须为 3 分量；密钥属于同一 context 且含 s² 对应键；输出 2 分量，level 不变 |
| `rescale_to_next(a, out)` | CKKS，2 或 3 分量，存在下一层；去掉当前末尾 q，scale 除以该 q |
| `mod_switch_to_next(a, out)` | BFV/BGV，2 分量，存在下一层；BGV 同时更新 correction factor |
| `set_scale(value, new_scale)` | CKKS；new_scale 有限、为正且小于当前 Q 的可表示上限；创建共享数据的元数据视图 |

`multiply` / `square` 不自动重线性化，所有操作均不自动降层或对齐 scale。
`set_scale` 不修改密文系数，使用它之前需要判断近似 scale 调整是否符合数学预期。

### 6.4 旋转方法

| 调用 | 槽位语义 |
| --- | --- |
| `rotate_vector(a, steps, galois_keys, out)` | CKKS 的 N/2 个槽位循环旋转；正数向左，负数向右 |
| `complex_conjugate(a, galois_keys, out)` | CKKS 槽位逐项复数共轭 |
| `rotate_rows(a, steps, galois_keys, out)` | BFV/BGV batching 的两行分别循环旋转；正数向左，负数向右 |
| `rotate_columns(a, galois_keys, out)` | BFV/BGV batching 的两行交换 |

旋转输入必须为 2 分量。整数方案要求 context 支持 batching。
步数必须满足 `0 < abs(steps) < N/2`，零步直接保留输入句柄。
当前前端要求 GaloisKeys 中存在本次变换的具体 Galois element，密钥准备见第 7 节。

### 6.5 inplace 方法

上述算术、重线性化、降层和旋转均提供 `_inplace` 变体，省略输出参数并更新第一个
密文句柄，例如：

```cpp
evaluator.multiply_inplace(y, x);
evaluator.relinearize_inplace(y, relin_keys);
evaluator.rescale_to_next_inplace(y); // CKKS
```

`set_scale` 直接更新句柄，没有额外 `_inplace` 名称。完整签名以
[evaluator.hpp](../../include/hpu/seal/evaluator.hpp) 为准。

## 7. 三方案的编程规则

### 7.1 Context 与输入准备

| 方案 | Context 入口 | 编码与输入形式 |
| --- | --- | --- |
| CKKS | `CkksContextSpec` + `create_ckks_context` | `seal::CKKSEncoder`，密文和导入明文均为 NTT 域 |
| BFV | `BfvContextSpec` + `create_bfv_context` | `seal::BatchEncoder`，密文和导入明文均为系数域 |
| BGV | `seal::EncryptionParameters(scheme_type::bgv)` + `seal::SEALContext` | `seal::BatchEncoder`，密文为 NTT 域，导入明文为系数域 |

`coeff_modulus_bits` 指完整 key-context 模数列表，包含数据 Q 和 special P。
CKKS 的每个 q/P 至多 32 bit；BFV 工厂要求 Q/P/t 至多 31 bit、恰好一个 special P
且 batching 可用。现有 BGV 前端案例使用三个数据 Q 和一个 P，参数定义见
[integer_context](../../examples/evaluator_application.hpp)。NTT、密钥切换等算子的
额外约束在构图或编译时检查。

CKKS/BFV 工厂使用 `sec_level_type::none` 的功能验证配置。第 2 节参数用于
快速验证，实际参数和验证状态记录在交付包中。

### 7.2 CKKS：按消费位置编码常量

乘法使 scale 相乘，rescale 去掉最后一个 q 并将 scale 除以该 q。
后续 AddPlain 的常量按当前值编码：

```cpp
seal::Plaintext native_bias;
encoder.encode(std::vector<double>(active_slots, 1.0),
               y.parms_id(), y.scale(), native_bias);
const auto bias = app.import_plaintext("constant/bias_next", native_bias);
evaluator.add_plain_inplace(y, bias);
```

两密文 Add/Sub 要求同 level、scale 兼容；当前前端 scale 相容检查的默认相对
容差为 `1e-6`。scale 必须有限、为正，乘法后还需小于当前 Q 的表示上限。
前端目前不支持 CKKS `mod_switch_to_next`，不能用它自动拉齐不同层级的分支。

### 7.3 BFV/BGV：整数明文与后处理

整数运算的数学语义位于模 t。常见乘法后处理为：

```cpp
evaluator.multiply(x, mixed, y);
evaluator.relinearize_inplace(y, relin_keys);
evaluator.mod_switch_to_next_inplace(y);
evaluator.add_plain_inplace(y, bias);
```

BFV 明文由前端按消费者分别准备 Add/Sub 的系数域缩放形式或 MultiplyPlain
的 NTT 形式，同一个导入明文可以复用。BGV 明文还按消费者的 correction factor
准备；factor 更新和平衡由前端处理，应用可通过 `correction_factor()` 查询。

现有 BFV/BGV 前端示例实现 `x*(RotateRows(x,1)+RotateColumns(x))+7`。
其计算图在各自 cpp 中定义，测试辅助代码提供输入和这一公式的数学预期。
复制示例后改公式时，也要更新 [integer_main](../../examples/evaluator_application.hpp)
中的预期计算，或在自己的程序里独立实现预期检查。

### 7.4 评估密钥与旋转键

重线性化键通过 `keygen.create_relin_keys(relin_keys)` 生成。
Galois key 应包含每次旋转所需的具体 element。例如 CKKS 左移一步与共轭：

```cpp
#include "scheme/ckks/galois.hpp"

seal::GaloisKeys galois_keys;
keygen.create_galois_keys(std::vector<std::uint32_t>{
    hpu::scheme::ckks::rotation_galois_element(N, 1),
    hpu::scheme::ckks::conjugation_galois_element(N)}, galois_keys);
```

BFV/BGV 的行旋转和行交换使用同一 generator-3 映射：

```cpp
#include "scheme/bfv/galois.hpp"

seal::GaloisKeys galois_keys;
keygen.create_galois_keys(std::vector<std::uint32_t>{
    hpu::scheme::bfv::row_rotation_galois_element(N, 1),
    hpu::scheme::bfv::column_rotation_galois_element(N)}, galois_keys);
```

上面代码假定已有同一 context 的 `keygen` 和对应 `N`。修改旋转步数时同步修改
密钥列表。只有一些其他步数的键不能满足当前前端的具体 element 检查。

## 8. 编译选项与诊断

```cpp
hpu::seal_adapter::CompileOptions options;
options.optimize_resources = true;
options.require_instruction_verification = true;
const auto compiled = app.compile("my_application", options);
```

| 字段 | 默认值 | 用途 |
| --- | --- | --- |
| `capacity_lines` | `1ULL << 24` | 数据构建阶段的行数上限，1 line=256B；实际包按使用量收紧窗口 |
| `optimize_resources` | `true` | 比较原序、读取复用、依赖重排候选，按 DMA 字节/次数/指令数选择 |
| `require_instruction_verification` | `true` | 最终指令模型与 golden 不匹配时停止编译 |
| `instruction_ntt_contract` | `natural_coefficient_boundaries` | 新应用的默认 host NTT 功能契约 |

选择 NoOpt 使用 `optimize_resources=false`，使用统一 SSA 后端生成原序基准，不承诺
保留历史手写 DMA、PFREE 或 DDR 布局。保留失败指令报告使用
`require_instruction_verification=false`。诊断模式仍执行和记录指令比较，SEAL 与
方案模型必须通过，资源优化改变基准结果也会失败。

关闭资源优化时，交付编译仍按同一规则设置 CP：仅 twiddle DLOAD 为 1，
其他 DMA 为 0。缓存策略与资源优化分别处理，不改变顶层编译选项或数学结果。

当前新应用使用自然系数边界与显式 twist/untwist PMUL。`legacy_raw_stage`
用于回放旧 raw 镜像，选择它不转换镜像或表，也不改变硬件 ABI。完整约定和
诊断 CLI 见 [前端入门](../getting-started/HPU_EVALUATOR_FRONTEND.md)。

### 8.1 IO 对照与示例参数

仓库三个 Evaluator 示例支持 `--degree N`、`--data-moduli COUNT`、`--seed UINT64`、
`--no-opt` 和 `--emit-dir PATH`。`--data-moduli 4` 表示 Q4，另准备一个 special P；
默认仍为 Q3。`--seed` 是可重复测试 fixture 选项，不改变应用接口。

```sh
./build/hpu_ckks_evaluator_example --degree 8192 --data-moduli 4 --seed 0 \
  --emit-dir outputs/ckks_evaluator_application_n8192_q4
./build/hpu_ckks_evaluator_example --degree 8192 --data-moduli 4 --seed 0 --no-opt \
  --emit-dir outputs/ckks_evaluator_application_n8192_q4_noopt
```

以 `compiler_resources.dma_bytes` 为主要 IO 指标，分别检查 `dload_bytes`、
`dstore_bytes`、`repeated_range_read_bytes` 和 `io_by_allocation`。`spills=0` 不能
说明没有重复读取。比较时保持相同参数、输入和输出集合；删除额外输出带来的收益
应单独计算。初始镜像体积与运行期 DMA 分开统计。

需要一次生成三方案并汇总时，使用 `python3 tools/benchmark_io.py --build-dir build
--output-root outputs/io_optimized`。默认 N8192、Q4、P1、seed=0，结果在输出根的
`io_summary.json`。用另一输出根加 `--no-opt` 生成原序报告，再通过
`--baseline-json PATH/io_summary.json` 比较；参数、fixture 或导出输出不一致会
拒绝比较。完整命令与统计口径见
[前端入门的 IO 对照](../getting-started/HPU_EVALUATOR_FRONTEND.md#n8192q4p1-的可重复-io-对照)。

## 9. golden、数学验证与交付

### 9.1 自动对照与用例预期

`compile()` 自动运行 SEAL oracle、方案 SoftwareExecutor 和最终指令 host 模型。
方案 SoftwareExecutor 的每个节点与 SEAL oracle 的有效 uint32 word 严格比较。
部署 golden 只保存指定导出结果，最终指令模型检查这些结果；优化模式还比较
相同输出集合的统一后端原序基准与最终优化流。

| 检查 | 用于回答 |
| --- | --- |
| 元数据和 RNS 密文字值逐节点相等 | 两种实现是否执行同一图并产生一致结果 |
| 解密后与独立公式比较 | 所构建的图是否表达了用例作者想计算的公式 |
| 目标端读回与 golden 比较 | 真实 HPU 是否产生该包要求的输出 |

CKKS 密文字值对照仍是严格相等，`1e-2` 等容差用于解密后的浮点数学预期。
BFV/BGV 解码预期按模 t 计算。自动对拍不能替代作者的独立公式检查。

第 2 节演示附加 CKKS semantic 报告。整数方案可使用
`integer_delivery_semantics(decoded)`；先自行检查数学预期，再写入报告。
这些辅助函数只组织报告，不替调用者完成预期断言。

### 9.2 输出文件与 IT 消费

| 文件或目录 | 用途 |
| --- | --- |
| `package.json` | 方案、案例名称及各清单路径 |
| `program/` | 最终指令、DMA 清单、目标侧 C/h；提供 `hpu_run_<stem>()` |
| `memory/hpu_mem_image.u32.bin` | 执行前加载的初始镜像 |
| `golden/golden_manifest.csv` | 指定导出结果的分量、模数、结果路径与最终读回 span |
| `metadata/operation_graph.json` | 依赖图与命名输出，供失败定位 |
| `oracle/report.json` | 功能对拍、最终指令验证、资源报告及优化等价状态 |
| `semantic/decoded.json` | 可选的用例数学验证报告 |

BFV golden 是自然顺序系数域；CKKS/BGV 是 canonical HPU NTT 物理布局。
golden 按导出节点、密文分量和模数 limb 拆分，仅这些结果保留目标读回空间。
计算图中 `exported=false` 的节点只有逻辑引用，没有保证可读的最终 DDR 地址；
`exported=true` 的节点通过 `golden_object_id` 关联 golden manifest。
host 指令对拍检查有效字值，IT 还应检查 padding 和越界写入。

目标端加载初始镜像并运行 C 入口，按 golden 清单读回和比较。golden 文件作为
期望结果，不加载到 HPU_MEM。`.inst32` 本身不是完整 ELF/BIN，Nexus-AM 需要
消费 C wrapper 或等效实现 DMA 参数。host PASS 与 RTL/上板验证分别记录。

详细数据协议见 [应用包 V1](../delivery/HPU_APPLICATION_PACKAGE_V1.md)，目标端
执行顺序见 [IT 交接清单](../delivery/IT_HANDOFF_CHECKLIST.md)。

## 10. 常见错误与排查

构图和参数错误通常抛出 `std::invalid_argument`；软件比较失败通常抛出
`std::runtime_error`，包输出也可能抛出文件系统异常。建议在生成程序 `main()`
捕获 `std::exception`，打印 `what()` 并返回非零。

| 现象或错误信息 | 检查与处理 |
| --- | --- |
| `hpu/seal/evaluator.hpp` 找不到或链接符号缺失 | 开启 `HPU_ENABLE_SEAL_INTEGRATION`，在 SEAL 目标定义后添加应用目标并链接 `hpu_seal_delivery` |
| `frontend name...` / `name is duplicated` | 检查名称字符、保留名称，以及输入和明文的重复导入名 |
| `belongs to another application or is empty` | 确认输入已赋值且来自当前 builder，SEAL 数据需先导入 |
| `does not support this component count` | 当前操作多数要求 2 分量，Multiply/Square 后先 Relinearize |
| `binary operands require matching levels` | 查询两个 `parms_id()`，按方案显式安排降层与分支合并 |
| `requires matching scales` / `plaintext and ciphertext levels differ` | 查询 CKKS level 与 scale，按实际消费者重新编码明文；判断是否需要有数学依据的 scale 视图 |
| `scale is invalid or exceeds the active modulus` | 检查乘法后的 scale、模数链与计算深度，调整输入参数或操作顺序 |
| `requested Galois key is missing` | 为实际步数或共轭/行交换准备具体 element 的键 |
| `ciphertext has no next level` | 当前已到链底，减少降层次数或增加所需数据模数 |
| `BFV multiply requires at least two active Q moduli` | 调整 BFV 模数链，确保乘法发生时仍有至少两个数据模数 |
| `application has no computation` / `marked output must be produced by an operation` | 添加计算节点并标记其结果，导入和 set_scale 视图不单独构成计算 |
| 输出目录已存在 | 使用新目录或第 3.3 节生成脚本 |
| `delivery SEAL/model mismatch` | 功能模型与 oracle 不一致，查看报错 allocation 和 word 下标；生成不会通过 |
| `encoded instruction verification...` | 定位最终指令与 golden 差异；需要保留报告时显式诊断导出并查看 `host_instruction_model` |

可通过 `compiled.resource_report_json()` 查看工作区和对象调度。不要凭旧 Plan
示例打印的基准地址读取优化包；使用最终交付清单。

## 11. 完整案例与进阶入口

| 目标 | 文档或代码 |
| --- | --- |
| CKKS 分支、旋转、共轭、乘法和降层 | [ckks_evaluator_application.cpp](../../examples/ckks_evaluator_application.cpp) |
| BFV 分支、行旋转、行交换和乘法 | [bfv_evaluator_application.cpp](../../examples/bfv_evaluator_application.cpp) |
| BGV 分支、correction factor 和降层 | [bgv_evaluator_application.cpp](../../examples/bgv_evaluator_application.cpp) |
| 查询全部顶层签名 | [evaluator.hpp](../../include/hpu/seal/evaluator.hpp) |
| 操作 Plan、数据镜像和底层算子 | [底层 API 参考](HPU_LOW_LEVEL_API_REFERENCE.md) |
| 理解资源 IR、runtime 和编译器边界 | [项目架构](../architecture/HPU_ARCHITECTURE.md) |
| 修改 SEAL 适配或对齐数学实现 | [SEAL 集成设计](../architecture/HPU_SEAL_INTEGRATION.md) |
| 查询 ISA 与 NTT 软件契约 | [HPU 编程手册](HPU_PROGRAMMING_MANUAL.md) |

新用例从对应前端示例或本手册的完整程序开始，修改计算图、输入、参数和数学
预期后重新生成完整包。实际支持能力以当前头文件、构图校验与编译验证结果为准。
