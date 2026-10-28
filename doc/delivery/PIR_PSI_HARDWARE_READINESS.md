# PIR/PSI 应用包与 Nexus-AM 接入审计

参考固定版本：inline-asm 原作者 `cfee951e4056660e521d55a3535d82335a49bb08`；Trident `e3f819f845f3cee7425fa08a3aa64879cfd5b3c0`；Nexus-AM `631f9970003b27ca82eb13549c830c02bea78215`（只读）。本轮没有修改 Nexus-AM、RTL、ISA 或编译器核心。

## 既有 19 包逐包清单与验证

[PIR_PSI_PACKAGE_MANIFEST.csv](PIR_PSI_PACKAGE_MANIFEST.csv) 给出 2 个 PIR 和 17 个 PSI 包的名称、分区号、输入对象数量与种类、导出 object ID、指令、DMA、`used_lines`、`capacity_lines`、镜像字节数和 Golden 清单路径。每包完整输入 allocation ID 位于该包 `memory/line_map.csv` 的只读行，查询密文、明文、评估密钥、twiddle 和常量均可按 `kind` 定位。输出由该包 `metadata/operation_graph.json.marked_outputs` 映射到 `golden/golden_manifest.csv` 的 `step_*`、组件、模数及目标 line；不能把不同包中同名 `step_*` 混为同一输出。

PSI 顺序为 `hpu_query_partition_0` 至 `_16`，包名分区号与 `pipeline_manifest.json.execution_pipeline` 一致。每包自含初始镜像、C/H、ASM/inst32、DMA、内存 ABI、Golden、来源和软件 oracle 报告；没有前包中间结果输入。单包可独立装载、执行、用本包 Golden 逐 word 校验。第 0–14 包各 2 个输出，第 15 包 2 个，第 16 包 1 个；全包 CPU 按接收方 Kuku 查询表中的位置 `location` 算 `bundle=location/items_per_bundle`、`base=(location%items_per_bundle)*5`，对每个分区已占用 bundle 的五个相邻解码字段判零；每包产生 raw item 匹配，再排序、去重、取并集。该规则是 Trident 五字段概率匹配，不能以原始集合交集过滤。小规模包与原生 SenderDB 差分可先验收该恢复流程。目标侧需要保存每包 `partition/bundle/group` 到输出 span 的映射，以便报错定位。

原作者 `hpu_validate_package` 本轮对 19 个**既有**包逐个返回 0，明细见[PIR_PSI_PACKAGE_VALIDATION.tsv](PIR_PSI_PACKAGE_VALIDATION.tsv)。清单工具还核对 package v1 必需路径、镜像长度、指令中 DMA opcode 数与清单行数、标记输出与 Golden 数量和顺序。未修改 JSON、Golden 或校验和；未重新生成正式包。19 包的 `provenance/build.json` 均为 `96837ad64523fdd181df699c416a7a22d23363db`、`dirty-at-configure`，所以它们能用于**静态兼容审计**，尚不符合[原作者 IT 清单](IT_HANDOFF_CHECKLIST.md)要求的干净生产提交。现有形式不能宣称由本分支重新生成。

## ABI 与内存

实际包字段为 schema v1、32-bit 小端 word、64 word/256 B line、`hpu-objects64-v1.2`、64 对象、`mod_rel_cp` DMA、x10/x11 寄存器、twiddle-only CP、NTT 数据 ABI v4、K8 双 line stage 和在线 PMUL 模板、BFV `coefficient_bit_reversed` 输出。原作者校验器核包内格式；Nexus-AM 固定[接收器](https://github.com/cuihu2/nexus-am/blob/631f9970003b27ca82eb13549c830c02bea78215/tests/hputest/scripts/import_package.py)对已收录用例还执行机器字、DMA、layout、golden、只读区/guard 和宿主模型门禁。本轮没有用 Nexus-AM 接收器导入 PIR/PSI：其 `package_profile`/`cases.tsv` 只接受固定 03–07 用例名和配置。该白名单是现有测试范围，不证明 PIR/PSI ISA 不兼容；强 ABI 检查仍须保留。

| 对象 | HPU_MEM `used=capacity` | 初始镜像 | 当前 Nexus-AM 情况 | 直接接入 |
|---|---:|---:|---|---|
| PIR stage0 最大单包 | 2,016,645 line | 516,261,120 B | 06 功能用例 `0x87000000`、256 MiB RAM，接收器 workload 预算小于 `0x07000000` B | 否；容量与固定用例均不合 |
| PIR stage1 | 662,405 line | 169,575,680 B | 超出 06 功能用例约 112 MiB workload 预算 | 否；还需 CPU 阶段交接 |
| PSI 最大单包（分区 0–14） | 369,457 line | 94,580,992 B | 单包镜像在 06 预算内，但 17 包及其名称/参数不在清单 | 否；先新增接收映射和调度 |
| PSI 17 包合计 | 5,980,417 line | 1,530,986,752 B | **不是同时所需 HPU_MEM window**；可顺序复用窗口 | 不适用 |

先前报告将 `5,980,417` 行写作 PSI 最大**单包**，与当前包 `hpu_mem_config.json` 不符；这是 17 包**总和**。PIR 两包目录总文件约 1,162,891,270 B；PSI 17 包目录总文件约 6,202,202,010 B（`du -sb`），均含指令、Golden 和元数据，不能当成内存镜像或 window。Nexus-AM 07 性能用例声明 512 MiB RAM、DDR 基址 `0x90000000`，其可用段受该基址及接收器公式约束；不能拿 512 MiB 数字直接批准 PIR stage0。目标 RTL/SoC 的真实 DDR、窗口上限与运行时额外开销目前没有完整配置，硬件承载能力仍待确认。

## Nexus-AM 交接契约

Nexus-AM 固定[README](https://github.com/cuihu2/nexus-am/blob/631f9970003b27ca82eb13549c830c02bea78215/tests/hputest/README.md)、[cases.tsv](https://github.com/cuihu2/nexus-am/blob/631f9970003b27ca82eb13549c830c02bea78215/tests/hputest/cases.tsv)、[build.py](https://github.com/cuihu2/nexus-am/blob/631f9970003b27ca82eb13549c830c02bea78215/tests/hputest/scripts/build.py)、[06 C 壳](https://github.com/cuihu2/nexus-am/blob/631f9970003b27ca82eb13549c830c02bea78215/tests/hputest/src/06_applications/06_01_ckks_x2_add1_n8192.c)显示：接收器复制原生 C/H/inst32 与镜像，生成 layout/golden/writable；C 壳准备镜像、提交 window、武装 IRQ、调用具名程序、等待唯一末尾 PSYNC，再逐 word 比结果及只读/guard；构建后反汇编核 ELF 中机器字与原生包相同。默认交付仅 ELF/BIN，源码留在仓库。该流程的软件模型或 ELF 检查不等于 RTL 通过。

| Nexus-AM 所需 | 当前 PIR/PSI | 差距与责任 |
|---|---|---|
| 每包 v1 原件、ABI、DMA、Golden、来源 | 19 包都有；旧产物来源 dirty | 应用方在干净提交重新生成正式包并存 SHA-256，不改旧 JSON |
| 06 组用例清单和独立 C 壳 | 无 PIR/PSI case ID | Nexus-AM/IT 新增固定用例映射；不放宽 ISA/Golden 强校验 |
| IRQ/PSYNC、窗口、逐 word 自检 | 包含程序和 span；目标 C 壳未写 | IT 为小包先做单包壳，再复用到 PIR 两阶段与 PSI 循环 |
| PIR CPU 分解及阶段二镜像 | 软件应用有算法和外部读回适配入口 | 软件执行器独立内存导出、目标 DSTORE 回传和动态重装接口待 HPU/IT 确认；可先用两个独立 C 入口调通，再做一个总控主程序 |
| PSI 17 包顺序运行与集合合并 | `pipeline_manifest` 和逐包 CSV 已描述 | IT 实现循环、每包错误码/分区定位及接收方五字段判零汇总 |
| 内存/运行时空间 | 包内精确用量已列 | 硬件团队确认可映射窗口、DDR 与额外缓冲；尤其 PIR stage0 |

分级联调：①小规模 PSI 单包；②小规模 PIR stage0；③小规模 PIR stage1 固定输入；④小规模 PIR 两阶段读回与 CPU 分解；⑤正式 PIR（先解决 516 MB stage0 window）；⑥正式 PSI 17 包顺序执行并逐包定位。每一级先用已有 profile 生成匹配包，再纳入 Nexus-AM 固定用例；不得借旧包手改 JSON 或压缩数据绕过容量。
