# HPU 项目架构

本文说明当前代码从应用接口到 HPU 交付包的完整流程，覆盖 CKKS、BFV、BGV，
并包含原《编译器资源管理》的资源 IR、DDR 复用、对象调度和验证说明。

## 1. 项目定位与阅读入口

当前项目是基于 C++ 嵌入式接口的 FHE/HPU 专用图编译器，同时提供 SEAL 适配、
host 软件验证和应用交付工具。程序员使用 SEAL 风格接口构建计算图，编译器将其
降低为 HPU 指令，并生成执行所需的数据镜像、DMA 清单和目标侧 C 调用入口。

| 读者任务 | 入口 |
| --- | --- |
| 编写应用或测试用例 | [Evaluator 前端入门](../getting-started/HPU_EVALUATOR_FRONTEND.md)，然后阅读对应方案的前端示例 |
| 理解分层、编译过程和 runtime | 本文第 2～5、7 节 |
| 新增算子或优化内存、对象调度 | 本文第 6 节及 [HPU 编程手册](../reference/HPU_PROGRAMMING_MANUAL.md) |
| 对接 Nexus-AM、执行与读回 | 本文第 9 节及 [应用包 V1](../delivery/HPU_APPLICATION_PACKAGE_V1.md)、[IT 交接清单](../delivery/IT_HANDOFF_CHECKLIST.md) |
| 维护 SEAL 数据与方案语义适配 | [SEAL 集成设计](HPU_SEAL_INTEGRATION.md)及 [底层 API 参考](../reference/HPU_LOW_LEVEL_API_REFERENCE.md) |

## 2. 整体流程

```mermaid
flowchart TD
    A[SEAL：上下文、密钥、编码与加密] --> B[ApplicationBuilder / Evaluator]
    B --> C[逻辑计算图]
    C --> D[方案适配：Plan 与数据镜像]
    L[直接使用现有 OperationPlan] --> D
    D --> E[算子资源 IR 与 DMA 端口绑定]
    E --> F[公共资源编译器]
    F --> G[最终指令、DMA、目标侧 C 与数据镜像]
    G --> H[应用交付包]
    H --> I[Nexus-AM / 目标工具链]
    I --> J[目标二进制与 HPU 执行]
    C --> V[SEAL oracle]
    D --> W[方案 SoftwareExecutor]
    G --> X[最终指令 host 模型]
    V --> Y[元数据、逐节点字值与优化等价性检查]
    W --> Y
    X --> Y
    Y --> H
```

整个流程分为两个阶段：

1. **host 构图、编译与交付**：运行应用生成程序，记录计算图、准备数据、
   分配资源、生成指令，并完成软件对拍。
2. **目标端编译与执行**：IT 使用 Nexus-AM/目标工具链编译生成的 C，配置
   HPU_MEM、加载初始镜像、执行程序，再读回结果与 golden 比较。

前端的 `compile()` 完成第一阶段的编译与验证，`write()` 将结果写入交付目录。
目标端执行证据由 IT 单独提供。

## 3. 代码分层与职责

| 层次 | 主要代码位置 | 职责 |
| --- | --- | --- |
| 应用前端 | `hpu/seal/evaluator.hpp`、`src/hpu/seal/evaluator.cpp` | 导入输入、记录逻辑值与依赖、校验元数据、标记输出和发起编译 |
| SEAL 与方案适配 | `include/hpu/seal/`、`src/hpu/seal/` | 处理上下文和 level，将 SEAL 对象转换为 HPU 数据布局，构建 Plan、常量、密钥、twiddle 与工作区 |
| 方案算子 | `include/scheme/`、`src/scheme/` | 实现 CKKS/BFV/BGV 的算子组合、域转换与方案专有后处理 |
| 公共算法组件 | `operator/`、`poly/`、`util/` 对应的 include/src | 组合 KeySwitch、CMult、ModUp/ModDown、NTT 和逐点运算，发出资源操作 |
| 资源 IR 与编译后端 | `hpu/compiler/operator_resources.hpp`、`resource_ir.hpp` 及对应 cpp | 建立虚拟对象与缓冲区，分析生命周期，分配对象槽位、bank 和 DDR，生成最终资源操作 |
| 公共交付编译与包格式 | `include/hpu/delivery/`、`src/hpu/delivery/` | 连接资源编译、重定位、最终指令验证与 C 生成，写出和校验通用应用包 |
| 指令编码与目标侧 C 生成 | `encode/` | 定义、解析和编码 ISA，生成 32 位指令及带 DMA 参数的 C wrapper |
| 内存与生命周期基础设施 | `include/hpu/runtime/`、`src/hpu/runtime/` | 提供 HPU_MEM 镜像、对象状态约束及基础模运算功能模型 |
| 软件验证模型 | `hpu/seal/*software_executor*`、`hpu/model/` | 分别执行方案级算法与最终指令字，检查它们与独立 SEAL oracle 的一致性 |

三方案共用前端句柄、资源编译器和交付协议。各方案的数据准备和数学实现保留各自
规则，例如 CKKS 的 scale/rescale、BGV 的 correction factor 和 BFV 的乘法基转换。
具体支持范围以 [前端入门的支持表](../getting-started/HPU_EVALUATOR_FRONTEND.md#3-当前支持范围) 为准。

## 4. 程序员使用的顶层 API

### 4.1 接口与最小程序

新应用使用 `hpu/seal/evaluator.hpp`：

| 接口 | 主要方法与用途 |
| --- | --- |
| `ApplicationBuilder` | `import_ciphertext`、`import_plaintext` 导入输入；`evaluator` 获取操作接口；`mark_output` 命名输出；`compile` 编译 |
| `Ciphertext`、`Plaintext` | 表示图中的逻辑值，提供 `parms_id`、scale 等元数据查询 |
| `Evaluator` | `add/sub/multiply/square`、明文运算、重线性化、方案适用的旋转与降层操作及 `_inplace` 变体 |
| `CompiledApplication` | `write` 导出包；`verified`、`instruction_verified` 查询软件验证结果；`resource_report_json` 查看资源统计 |

下面假定已有有效的 `context` 和 SEAL `encrypted_input`，构建三方案均支持的加法应用：

```cpp
#include "hpu/seal/evaluator.hpp"

hpu::seal_adapter::ApplicationBuilder app(context);
auto evaluator = app.evaluator();
const auto x = app.import_ciphertext("input/x", encrypted_input);

hpu::seal_adapter::Ciphertext y;
evaluator.add(x, x, y);
app.mark_output("y", y);

const auto compiled = app.compile("my_add");
compiled.write("outputs/my_add"); // 直接写包要求目标目录不存在。
```

context、编码、加密、密钥生成和解密继续使用 native SEAL。应用向操作接口传入所需
评估密钥，前端自动准备对应的 HPU 密钥布局、常量、twiddle 和工作区。

完整程序与构建命令见 [Evaluator 前端入门](../getting-started/HPU_EVALUATOR_FRONTEND.md)，
示例为 [CKKS](../../examples/ckks_evaluator_application.cpp)、
[BFV](../../examples/bfv_evaluator_application.cpp)、
[BGV](../../examples/bgv_evaluator_application.cpp)。

### 4.2 构图与值语义

`Evaluator` 的方法记录计算图，host 执行、对拍和编译发生在 `compile()`。
调用顺序给出图的构造顺序，输入句柄给出数据依赖；同一输入可被多个节点引用，
因此能构造分支与汇合。当前后端按依赖生成静态指令序列。

句柄表示不可变逻辑值。复制句柄保留原值，`_inplace` 创建新值并重新绑定传入的
句柄，其他引用仍指向旧值。输入与评估密钥按快照捕获；后续修改原 SEAL 对象或
继续构图，不会改变已经编译的包。

前端保留 SEAL 风格的显式操作：`multiply` 后的重线性化、rescale、mod switch
和 scale 调整均由应用选择。`set_scale` 是共享数据的元数据视图，不发出算术指令。

多个结果可以通过 `mark_output` 命名。部署默认只保留全部标记结果；没有标记时保留
最后一个计算结果。内部节点不为逐节点读回强制写回，显式标记中间值即可将其作为
可观察输出。多个标签或 scale view 引用同一物理值时不重复生成 golden。
`compiled.oracle_result(value)` 返回 SEAL 参考结果；实际 HPU 结果由目标端另行读回。

## 5. 从计算图到最终指令

`ApplicationBuilder::compile()` 位于 `src/hpu/seal/evaluator.cpp`，主要过程为：

1. 冻结图、输入和密钥快照，按图运行独立 SEAL oracle。
2. 根据 context 的 scheme 创建对应 Plan 和应用镜像，准备资源与输出元数据。
3. 用对应方案的 SoftwareExecutor 执行应用，与 oracle 比较各节点的元数据和字值。
4. 展开方案算子，收集原生 `OperatorProgram`，并准备与镜像关联的 DMA 端口。
5. 公共交付编译器先规范化内存身份，执行 SSA 读取合并、写读转发和死值消除，
   构造依赖调度候选；再绑定为 `VirtualProgram`，统一安排按需加载、bank、
   释放、spill/reload 和 DDR，重定位 DMA 与指定输出的 golden，生成最终 C/指令。
6. host 指令模型执行最终指令字与实际 DMA span，与 oracle golden 比较。
   优化模式还比较统一后端原序基准与所选优化流的全部导出结果。
7. 返回持有编译快照、包请求和报告的 `CompiledApplication`。

公共交付编译入口为 `hpu/delivery/resource_compiler.hpp` 中的
`compile_application_resources()`；`src/hpu/seal/frontend_package.cpp` 将前端连接
到该公共入口。

现有 `CkksOperationPlan`、`BfvOperationPlan`、`BgvOperationPlan` 仍可直接使用。
它们创建的应用包携带原生资源程序，`write_application_package()` 在私有镜像上
完成相同的优化、重定位和严格指令对拍。调用者的 Image/Plan 保持原布局，已经编译
的前端包直接导出。只含原始编码指令的历史请求继续使用兼容导出路径。

旧 Plan 示例打印的 `--print-asm` 和指令计数属于兼容基准；最终交付指令及地址
以包中的 `program/*.asm`、`*.inst32` 和 `dma_relocation_manifest.csv` 为准。

## 6. SSA、IO 优化与统一资源管理

### 6.1 两层 IR 的职责

| IR | 表达内容 | 不负责 |
| --- | --- | --- |
| `OperatorProgram` | 单次定义的 `ValueId`、逻辑数据端口、计算依赖、模数上下文和原地约束 | 物理对象号、bank、硬件释放时机 |
| `VirtualProgram` | 存储对象、DDR backing/range、驻留和生命周期、资源约束 | 重新定义方案数学规则 |

继续使用这两层，不再增加一套与之平行的指令模型。优化必须在 SSA 值仍独立时完成；
降低为原地存储后直接删除 DLOAD，可能破坏 NTT 或 PMAC 的旧值。

### 6.2 原生算子的表达

算子通过 `make_operand(label)` 建立任意数量的独立变量，同名也具有不同身份。
`Recording` 为输入、算术结果和每个 NTT stage 创建新的 SSA 版本。

```cpp
namespace r = hpu::resource;
r::Recording recording;
const auto a0 = r::make_operand("a0");
const auto b0 = r::make_operand("b0");
const auto t0 = r::make_operand("t0");
r::read(a0, hpu::DataType::poly, "input/left/c0/mod0");
r::read(b0, hpu::DataType::poly, "input/right/c0/mod0");
r::multiply(t0, a0, b0);
r::output(t0, "output/c0/mod0");
auto program = recording.finish();
// 数据准备层为上述逻辑端口绑定实际 allocation 和范围。
```

`read/input` 声明逻辑数据来源，`output` 声明逻辑写入，不决定目标机器的即时 DMA
或 REL。算子无需按固定容量反复读取同一输入，也无需手动 `discard`。
兼容接口仍可渲染历史汇编；兼容槽号只供这种渲染使用，编译器不据此分配资源。
`publish/discard` 的兼容语义在规范化时转成逻辑写入和自动生命周期。

端口保存稳定的 `logical_binding`，可使用 `bind_named_ports()` 或已解析的 `input`
绑定数据；`bind_port/bind_ports` 用于连接已有适配请求。逻辑身份不依赖最终 DMA
序号。程序片段 `append()` 重编号值和端口，并连接显式 live-out。
`render_operator_ir()` 可查看这些值、端口和数据依赖。

### 6.3 数据身份、内存版本与复用

绑定后按规范化的 allocation、精确子范围、内存版本、表示和加载约束识别数据。
只读输入、明文、密钥、twiddle 可在多次引用之间复用。写入使重叠范围的缓存身份
失效；完全匹配的写后读可以转发 SSA 值，部分覆盖保守处理。

优化按以下顺序进行：

1. 去除算子中的物理释放约定，把所需输出作为持久化根。
2. 合并可证明相同的读取，将后续 uses 指向统一值。
3. 转发内部完整写读，删除不必要的 DDR 往返。
4. 从指定输出和必要副作用反向标记存活节点，删除死计算与未用数据。
5. 重算 uses/最后使用，再降低为存储对象。

统一值身份不等于永久驻留。一个值可以被释放后重新从有效 backing 加载；容量
充足时才保持到后续使用。NTT/PMAC 的 `tied_input` 保留原地硬件约束，旧值仍存活
时优先利用已有 backing；确实没有可用副本时才生成精确 uint32 snapshot。

### 6.4 调度候选与选择

编译器构造原序、复用和重排三个候选，均使用同一个资源后端。
重排使用确定性的 list scheduling，遵守 SSA、内存读写、模数上下文和屏障依赖，
按预计新增 IO、瞬时存储压力及原序打平选择就绪计算。每个多项式内部的 NTT stage
依赖不变；独立多项式可以共享 stage 参数，不按源代码段边界强制释放。

运算记录实际 `modulus_context`，最终按调度次序补充必要 PMODLD，避免把隐含的
模数选择移错。完整程序末尾仍统一 PSYNC，不新增逐 stage CPU 等待。

三个候选按最终 **DMA 总字节、DMA 次数、指令数** 依次比较，选出代价最小者。
`optimize_resources=false` 选择统一 SSA 后端生成的原序基准；它不再承诺保留早期
手写 DLOAD/PFREE 流的机器码、布局或每条 DMA。单独的 encoded-import ISA 回放
仍可保留显式原始编码，不能与应用 NoOpt 混为一谈。

### 6.5 片上对象与 bank 分配

64 个逻辑对象槽位 p0…p63 与物理 bank 独立。Bank 0…4 各 1024 line，普通对象
按 `128 * next_pow2(ceil(actual_len/128))` 保留对齐区间。N8192 的单模多项式为
128 line，普通区理论容纳 40 个；二分量 Q4 密文占 8 个这样的多项式。
执行时还需要常量、密钥和新结果的瞬时空间，不能只按完整密文个数判断可行性。

编译器与指令模型共用 `runtime::BankAllocator`。Bank 5 前 4 line 固定留给模表，
其余可容纳 14 个两条 line 的 stage 参数。mask 仅约束当前 DLOAD；CP 不参与
SRAM 容量或生命周期计算。PMAC 和 NTT 先申请新结果，再释放旧区间；NTT 排除
输入 bank。具体硬件规则见 [Bank 分配指南](../reference/HPU_BANK_ALLOCATION_GUIDE.md)。

后端先回收死值，再尝试实际放置。需要淘汰时，只选择能缓解当前 bank/mask/区间
失败的候选，结合写回/重载字节和下一次使用距离决定：干净输入直接释放，脏且仍
存活的值才 spill。对象槽位峰值低也可能因 bank 限制溢出；`spill=0` 也可能存在
重复输入 DMA，两者应分别统计。

PFREE、DSTORE REL 和 reload 由后端生成。必要输出写回后如还有消费者，可以
继续驻留，不因算子边界强制释放。PE 内部的 128 个 NTT 寄存器仍由硬件调度。

### 6.6 DDR 与输出物化

只有实际使用的输入、只读资源、必要 backing 和指定输出进入部署镜像。未导出的
计算结果归编译器管理；值一直驻留且没有必要写回时，不为其保留独立 DDR。

可写 workspace 在所有读取前已完整定义，且使用区间不交叠时，可进入统一 scratch
arena。依赖初始零值的范围保持必要初始化。逻辑别名规范化后指向实际物理分配；
最终 memory manifest 只列物理对象，DMA 指向相应子范围。

前端默认只持久化全部标记结果，无标记则持久化最后计算结果。Plan 交付适配器
默认只持久化最终输出，也可显式传入输出前缀集合。host SoftwareExecutor 仍在
独立主机镜像中检查各步 oracle；它的中间镜像不是部署输出清单。

### 6.7 IO 与资源报告

`oracle/report.json.compiler_resources` 和 `compiled.resource_report_json()` 给出：

| 字段 | 含义 |
| --- | --- |
| `ir_source` | `operator` 为原生 SSA；`encoded_import` 为 ISA 兼容导入 |
| `original_ddr_lines` / `allocated_ddr_lines` | 输入镜像与最终物化布局的行数 |
| `original_dma_bytes` / `original_dma_count` | 相同输出集合的原序基准 IO |
| `all_checkpoint_dma_bytes` / `all_checkpoint_dma_count` | 保留全部计算节点 checkpoint 的诊断基准 IO |
| `output_policy_saved_dma_bytes` | 只收敛输出根、尚未做读取合并和转发时减少的实际 IO |
| `dload_bytes` / `dstore_bytes` / `dma_bytes` | 所选目标流实际 DMA 字节 |
| `required_output_write_bytes` | 导出 allocation 的实际写入字节；含必要的多次或分段写入 |
| `repeated_range_read_bytes` | 同地址范围的重复加载字节；含合法重载，仅用于定位热点 |
| `spill_write_bytes` / `reload_read_bytes` / `exact_copy_bytes` | 换出写回、重载及 SSA 精确副本的实际传输字节 |
| `removed_checkpoint_min_write_bytes` | 未导出中间 checkpoint 的最少写回量，不计入同输出集合的优化收益 |
| `initial_image_bytes` | 初始镜像体积，单独于运行期 IO |
| `selected_schedule` | `original`、`reuse` 或 `reordered` |
| `spills` / `reloads` | 换出次数 / SSA 对象首次装载后的再次装载次数 |
| `peak_regular_lines` / `peak_small_lines` / `peak_object_slots` | 片上占用和槽位峰值 |
| `logical_buffers` / `io_by_value` / `io_by_allocation` / `io_by_kind` | 逻辑到物理映射、布局、逐 SSA value、逐 allocation 和逐数据类别的 IO |

性能基准使用 N8192、Q4、P1；示例命令为 `--degree 8192 --data-moduli 4 --seed 0`。
加 `--no-opt`、保持相同输出集合可对照编译器收益。取消中间输出本身带来的 IO
变化应单独报告，不能计入同一输出集合的复用收益。CP=1 仅请求 LLC 分配，编译
代价仍按完整传输字节计算；功能模型不模拟缓存命中或硬件周期。

`tools/benchmark_io.py --build-dir build --output-root outputs/io_optimized` 统一生成
和校验三方案基准，把本次候选、最终 DMA 清单核对结果、逐数据类别 IO、参数和
导出输出集合写入 `io_summary.json`。可指定 `--baseline-json` 比较另一次真实
测量，仅接受参数、fixture、输出集合及 oracle payload 指纹一致的报告。比较结果
按数据类别列出读写 IO 差值，并单列精确副本、spill 和 reload 的前后字节。命令见
[前端入门](../getting-started/HPU_EVALUATOR_FRONTEND.md#n8192q4p1-的可重复-io-对照)。

## 7. runtime 的作用与边界

### 7.1 runtime 核心

`include/hpu/runtime/` 与 `src/hpu/runtime/` 提供执行所需的数据表示、内存模型和
生命周期约束：

| 模块 | 当前职责 |
| --- | --- |
| `HpuMemImage` | 构建确定性的 HPU 可见 DDR 镜像，管理 256B 行对齐的 allocation、span 和只读别名；提供初始数据与序列化来源 |
| `runtime::Application` | 记录与检查模表加载、对象驻留、脏状态、写回、释放和结束时的 `psync`；同时维护 domain、level 与 key domain |
| `HpuSoftwareExecutor` | 在 host 内存中执行读写、复制和精确 uint32 模运算，为方案软件执行器提供基础功能模型 |

`runtime::Application` 的 `run_kernel()` 记录事件与写入状态。该状态机目前主要由
适配辅助接口和测试使用；前端编译路径的自动资源分配由 `compiler` 后端完成。
状态机管理对象槽位生命周期，bank 容量由资源编译器和指令模型检查。

### 7.2 RuntimeProgram 与目标侧 C

`hpu/seal/operation_runtime.hpp`、`bfv_operation_runtime.hpp` 等接口将降低后的
程序组织为编码指令与有序 DMA span，生成目标侧 C/h。最终资源编译后，公共交付
编译器按最终布局重新渲染这些产物。

- `hpu_program_<stem>(spans, count)` 接收按 DMA 次序排列的动态 span。该入口保持
  原实现，运行时检查 span，并在每条 DMA 前分别设置 `x10` 和 `x11`。
- `hpu_run_<stem>()` 使用生成时已经解析好的固定 span。生成器先检查范围、bank
  容量和对象长度，再将整个 HPU 流放入一个连续的 `asm volatile` 块。每条 DMA
  前重新设置 `x10`；`x11` 只在首次 DMA 和长度变化时设置。
- `x10` 传 line offset，`x11` 传 line count，两者均以 HPU line 为单位。
  固定入口显式声明 `x10`、`x11` 和 `memory` clobber，连续汇编块中没有可覆盖
  `x11` 的 C 代码。HPU 指令字和执行次序保持不变。

这项优化发生在资源调度完成后的目标 C 生成阶段，使用最终的 DMA 次序及 span。
固定 span 表和应用包 V1 协议保持兼容；校验器也接受此前由固定入口调用动态入口
的 V1 源码。

host 逐指令模型直接使用 DMA span，不执行 CPU wrapper，因此不能验证目标代码
实际写入 `x10/x11` 的值。`hpu_target_dma_codegen_test` 将生成 C 编译为 RV32/RV64
目标代码，检查 CPU 执行时的 DMA 参数、HPU 指令顺序及 `x11` 设置次数。
工具链配置和执行命令见 [IT 交接清单](../delivery/IT_HANDOFF_CHECKLIST.md#31-目标-cpu-包装代码检查)。

目标平台需要由 Nexus-AM/IT 完成工具链编译、HPU_MEM 配置、初始镜像加载、启动
与结果读回。当前项目提供这一对接所需的程序和数据，设备驱动及平台运行环境由
目标集成负责。

### 7.3 软件执行模型

方案级 `CkksSoftwareExecutor`、`BfvSoftwareExecutor`、`BgvSoftwareExecutor` 按算子
算法执行同一镜像。`hpu/model/instruction_executor.hpp` 中的逐指令模型则解码最终
指令字，模拟 DMA、对象状态和算术操作。两者用于 host 验证，具体证据边界见下一节。

## 8. 验证路径与证据边界

### 8.1 三条计算路径

前端自动执行并比较：

```text
native SEAL 图执行
        ↕ 每个节点的元数据和字值
各方案 SoftwareExecutor
        ↕ 同一份 oracle golden
最终指令字 + 实际 DMA span 的 host 执行模型
```

SEAL oracle 独立使用导入输入和图操作计算。逐指令模型解码 `instruction.word`，
并从实际镜像读取 modulus、常量和 twiddle，独立于 companion `normalized_asm`
及语义对象字段。

| 状态或接口 | 含义 |
| --- | --- |
| `CompiledApplication::verified()` | SEAL 与方案 SoftwareExecutor 的元数据和字值比较通过 |
| `InstructionExecutionReport::completed()` | 指令与 span 执行完成 |
| `InstructionMemoryComparison::verified` / `CompiledApplication::instruction_verified()` | 最终指令模型的保留输出逐字匹配 oracle |
| `optimization_equivalence_verified` | 优化流与 NoOpt 的全部保留输出一致 |

执行完成不意味着与 oracle 一致。缺少 DMA、非法生命周期、只读区写入、bank
不足、未知 mode/flag 和数值不匹配分别给出诊断。优化等价检查验证调度与复用是否
改变原始流结果，SEAL 比较验证结果是否满足独立 oracle，两者分别记录。

严格编译默认要求三条计算路径通过。前端设置
`CompileOptions.require_instruction_verification=false` 可显式诊断导出，保留失败
记录；它仍要求 SEAL 与算子软件模型通过，并检查优化是否改变原始流结果。

用例作者还应提供独立数学预期，例如解密后与 `x²+1` 或旋转表达式比较，确认所构建
的图符合测试目标。私钥仅用于 host 解密，不进入交付包。

### 8.2 NTT 软件契约

`execute_ntt_stage` 执行供应表所指定的一个 raw loader/BF/P stage。逐指令层提供
两个功能软件契约：

| `InstructionNttContract` | 行为 |
| --- | --- |
| `natural_coefficient_boundaries` | 默认；PNTT stage 0 的 raw stage 前做数学边界 BR，最后一个 PINTT raw stage 后做数学边界 BR，完整变换以自然位置读写系数 |
| `legacy_raw_stage` | 保留旧 loader/BF/P stage，不增加整体边界转换；正向入口使用 BR 系数 |

两个契约都逐位置执行 PMUL，原样消费 DMA 和表。默认契约中的 BR 为软件数学
边界转换，不额外编码 BR 指令。三方案 builder 与 SoftwareExecutor 使用相同的
自然系数 pre/post 索引，twist、untwist 和归一化由显式 PMUL 完成。

当前 stage DMA 使用固定 512 B 的 K=8 在线生成参数。镜像构建时，公共
`pack_stage_twiddles` 从完整物理 oracle 表提取 U/V/R 并重构全表校验；逐指令层和
算子 SoftwareExecutor 从实际 DMA 参数调用 `expand_stage_twiddles`，再交给原
raw stage 数学模型。完整表 API 保留为离线 oracle，不再作为生产 src2 payload。
前端 API 与 pre/post 数学边界保持不变；当前使用六位对象编码，参数以 MOD=0、
显式 mask=0x20 放入 Bank 5；普通 bank 按 128-line 单元分配。应用交付编译在
最终布局确定后按 `AllocationKind::twiddle` 设置 DMA：twiddle DLOAD 为 CP=1，
其他 DLOAD 与全部 DSTORE 为 CP=0，覆盖 stage 参数、pre/post 向量和编译器
插入的 reload，NoOpt 模式也采用相同策略。底层 API 默认 CP=0、公共资源编译器
默认保留手工 CP；包内 `dma_cache_policy` 区分两种策略。K=8 参数内容继承先前迁移，
最新 ISA 与放置规则见 [编程手册](../reference/HPU_PROGRAMMING_MANUAL.md)。

完整公式、stage 表顺序及 raw inverse API 适配见
[编程手册的变换契约](../reference/HPU_PROGRAMMING_MANUAL.md#9-本项目的接入约定)，
数据 ABI 与旧包迁移见
[应用包 V1](../delivery/HPU_APPLICATION_PACKAGE_V1.md#新应用镜像的-ntt-表与软件边界)。
选择回放契约不修改镜像、DMA 或指令，新自然镜像使用 legacy profile 可能比较失败。

完整往返正确不能单独证明正向谱正确；验证应包含独立求值或 SEAL oracle，检查
canonical 与 modified-root 路径。实际测试规模和结果以测试日志、各包报告为准。
独立反例、CKKS 舍入修复和验收快照见
[2026-10 审计](../audits/HPU_NTT_CONTRACT_AUDIT_2026-10.md)。

### 8.3 目标验证

host 模型描述功能行为，尚未建立 RTL 每 stage 地址和周期的验证证据。报告记录
`ntt_contract`、`forward_boundary_conversions`、`inverse_boundary_conversions`，
并保留 `hardware_contract_verified=false`。

`instruction_execution_verified`、`rtl_verified`、`hardware_verified` 等目标状态
继续为 false；host 指令验证使用独立字段。目标执行与数学读回验收按
[IT 交接清单](../delivery/IT_HANDOFF_CHECKLIST.md) 记录。

## 9. 交付产物与 Nexus-AM 对接

通用包适用于三方案，主要目录为：

| 目录 | 内容 |
| --- | --- |
| `program/` | 最终 `.asm`、`.inst32`、`.cmd26`、C/h 入口和 `dma_relocation_manifest.csv` |
| `memory/` | 初始 `hpu_mem_image.u32.bin`、物理分配清单、行映射和内存 ABI |
| `golden/` | 指定导出结果及目标读回 span，用于结果比较 |
| `metadata/` | 加密参数、level 和计算图依赖 |
| `oracle/`、`semantic/` | 软件验证、资源报告和可选的解码语义结果 |
| `provenance/` | 生成版本、文件长度和校验和 |

`.inst32` 和 `.cmd26` 本身不携带完整 CPU DMA sideband，也不是 ELF/BIN。
目标工具链编译 C wrapper 及平台启动代码后生成目标二进制；也可等效实现 DMA
清单中的参数设置。运行时加载初始镜像，执行后按 golden span 读回与比较。
golden 作为期望结果保存，不能预加载到 HPU_MEM。

包格式、目录覆盖规则和批量生成命令以
[应用包 V1](../delivery/HPU_APPLICATION_PACKAGE_V1.md) 为准。`outputs/` 为生成目录，
正式交付应从同一源码 commit 重新生成并归档完整包。

## 10. 当前编译器能力范围

当前实现已经具备专用编译器的核心环节：

- 前端构图与语义检查。
- 算子分解和中间表示。
- 生命周期分析、DDR 复用、对象槽位与 bank 分配。
- HPU 指令生成、编码与 DMA 重定位。
- 目标侧 C、数据镜像及交付包生成。

它采用 C++ 嵌入式接口：普通 C++ 编译器先将用例编译为 host 生成程序，运行该程序
时构建并编译 FHE 图。用例中的普通循环、条件判断在构图时执行；当前计算图和
输出指令流是静态的。

编译器目前覆盖已实现的 CKKS/BFV/BGV 操作，支持范围见前端文档。资源优化以
指定输出为持久化根，支持读取复用、写读转发、依赖调度和 bank 分配，尚未提供
PE 寄存器分配。
目标 ELF/BIN 的生成及设备执行属于 Nexus-AM/目标工具链集成阶段。

因此，项目的软件编译链路已经成立，实际 RTL/HPU 执行验收是独立的目标验证阶段。
