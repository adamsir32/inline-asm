# HPU 64 对象：集成与编译器交接

> 同步说明：本项目以 2026-10-21 v1.1 手册与分配指南为准，已纠正原交接稿中的 PPLACE 和持久 mask 两处过时描述。下文 RTL 测试记录是提供方的历史记录，不代表本项目或 CPU/SoC 集成已通过。
日期：2026-10-21。按用户要求，只修改 `rtl/cpu/SCPU_RTL/RTL/hpu/rtl/` 设计。未修改 CPU、SoC、`RTL/latest/`、全系统 filelist 或外部编译器/Spike。

完整新 ABI 和容量以 [软件编程手册](../reference/HPU_PROGRAMMING_MANUAL.md) 为准。本说明可直接转交负责 CPU 集成和编译器的同事；本次未通过外部消息工具发送。

## 给编译器同事

1. 逻辑对象扩展为 p0…p63。计算 opcode 为 **0x5B**，DMA 为 **0x2B**。custom1 rs1/rs2 位置不变，其他字段重排；所有旧机器码必须重新编码。
2. AR3 使用 PD6/PS1_6/OP2_8/I；NTT 不再重复编码 PDATA，新增 PPARAM6。PMODLD 仍只带 MOD_ID8，模表固定 Bank 5。没有 PPLACE，OPC=9…F 保留且非法。
3. DLOAD 最后一个操作数从 small_bank 布尔改为六位 bank mask。TYPE=2 模表只接受自动或 Bank 5；TYPE=3 两条 line 在线参数默认 Bank 5。不要把旧 small_bank=1 直接写成新 mask=1。
4. **默认 Bank 5：4 line 模表=64 contexts，加 14 个两条 line 参数槽。** N=8192 一方向 13 stage 参数共 26 line，可全部常驻；不代表多个模数和两个方向的参数可全部常驻。若 R 改成 8/16，参数槽数相应变为 12/8。
5. 参数在 SRAM 中只读。每 stage 两次 line 读取，因子生成状态留在 HPU 的八条寄存器链中；无需软件逐批写回，无需为生成结果申请普通 bank。参数依赖 N、q、根、方向、stage、K=8。
6. 40 个 N8192 多项式是普通 SRAM 静态上限。NTT/PMAC 仍需额外结果区间，twist/scale 常量仍占普通 SRAM。预算按 128-line 向上取整容量和实际 bank 分布进行。
7. DLOAD mask 只约束本次装入，不保存为对象策略。NTT/PMAC 结果按 [v1.1 分配指南](../reference/HPU_BANK_ALLOCATION_GUIDE.md) 分配，无需 PPLACE。
8. PFREE 和 DSTORE REL=1 结束生命周期。REL=0 保留对象。软件必须处理 fault，不得在分配失败后继续消费该结果。
9. 推荐更新 assembler/parser、encoding 表、`.cmd26` 生成、runtime relocation 和软件参考模型，并以硬件字段表验证单条机器码。当前工作区没有完整 assembler/Spike HPU 模型源码，因此本轮不修改这些工具。

## CPU/SoC 所需后续修改（本轮未实施）

- 当前 `FullSys.f` 和 CPU 子系统清单编译的是 `RTL/latest/`。本轮修改不会自动进入当前 SoC 仿真；需要由集成流程将本目录改动移植/重新生成至有效源，再构建 SoC。不要混用两份同名 HPU module。
- 新增 `control/hpu_twiddle_stream.sv` 必须进入 HPU 编译/综合清单，且 `pe_opcode_pkg.sv` 在 PE wrapper 之前读取。
- 既有 IT ELF、twiddle 测试台的 c0/c1 编码 helper、3-bit 完成事件及旧 `.cmd26` 向量仍按旧 ISA。本轮保留它们，后续跑这些用例前必须迁移或重新生成；不能直接以旧测试失败判断数学路径退化。
- HPU 故障对象 ID 从 3 bit 扩为 6 bit。`HpuBringup`/BlackBox、`LNSystem` 保持寄存器与 CDC、`DevicesWrapper`、`TLDeviceBlock`、`TLDeviceBlockInner` 均需贯通六位，不能只改端口宽度而遗漏三个高位同步器。
- HPU 本地 CSR 已采用对象号 `[9:4]`。SoC CSR 镜像也要留出六位；当前 fault code 位于 `[15:8]`，与扩展后的对象位冲突，建议将 fault code 移到 `[23:16]` 并同步软件定义。VALID/IS_LOAD/W1C 保持原位。
- 核侧按 opcode 识别 0x5B/0x2B，保持 custom1 `[19:15]`/`[24:20]` 两个读寄存器字段；custom0 不应产生这些字段对应的 GPR 依赖。custom1 `[11:7]` 是 mask，不是 GPR rd。
- 命令必须按程序顺序合流，采用 `{kind,inst[31:7]}` 原样传递 custom1，并保持 sideband 与命令配对。现有核侧实际采用原样传递，手册中的旧 precode 重排不能继续沿用。
- 若 Chisel 重新生成 RTL，应把对象数、6-bit ID 接口、fault CSR 和新增 HPU 参数同步回源配置。不要只维护生成 SV 的局部补丁。

## 设计实现范围

- 引入 commit `32b106c13cc77b442f38a48c40785a25d25ffc98` 的 128-line 对齐二次幂普通分配方案，以及 NTT 多候选 bank 思路。
- allocator 增加 Bank 5 模表/小对象隔离、两条 line 对齐小对象、原子 reclaim 和错误响应。分配失败不会释放旧对象。
- ID 全链路六位：decoder、对象表、controller、执行完成、DMA 描述符、DMA 配置器事件和本地 fault CSR。
- Bank 5 默认 4-line 模表 aperture，可通过 `hpu_top_wrap.MOD_TABLE_LINES` 配置 1…16；它沿顶层传到 allocator/controller。
- 将已有 `RTL/latest/` 的在线 twiddle 生成器、stream 读路径和 PE DATA/GEN 标签机制接入原本缺少这些功能的 `hpu/rtl`，使 TYPE=3 两条参数真正可执行。未移植其他 DMA 优化。
- DSTORE REL 贯穿 HPU 内部 DMA 控制路径。REL=0 不清对象有效性，REL=1 成功完成后释放；load 失败使新对象失效并回收。
- 未引入原址 NTT/PMAC、完整 twiddle SRAM 缓存或在线 pre/post 因子乘。

## 局部验证

新增 `ver/it/core/Difftest/tests/objects64/tb_objects64.sv`，只连接 HPU controller/decoder/allocator/object table，以事件驱动模拟 DMA/执行完成，不实例化 CPU、真实 DMA IP 或 PE 数据计算。测试不修改既有 IT 用例和历史数据。

VCS 重跑入口：

```sh
bash ver/it/core/Difftest/tests/objects64/run.sh
```

本机 VCS 启动因许可证服务无法连接而失败，故没有运行 VCS/IT。使用本机 Verilator 对这个小型控制器测试进行补充验证，检查了：

- 40 个 128-line 分配区间互不重叠，覆盖全部 5120 个普通 line。
- 固定模表地址与 14 个独立两条 line 参数槽，Bank 5 最后一个槽的释放复用。
- 普通/小对象空间耗尽，以及失败 replacement 不损坏旧对象。
- 高位对象号与低位不混淆，p39/p63 NTT 操作数与六位提交。
- DSTORE REL 描述符与模拟完成后的保留/释放。
- 原交接稿记录过早期 PPLACE 测试；该项已被 v1.1 取消，不能用作当前验收证据。高位算术对象号和 cimm8=255 仍需核对。

PASS：`HPU_OBJECTS64_PASS normal=40 seeds=14 high_id=63 faults=3`。这不等于真实 DMA REL、NTT 数学结果、真实 SRAM 仲裁或 CPU IT 已通过；后续需在集成目录同步后完成这些验证。

PE 控制/生成器/真实算术 RTL 的 Verilator 语法与展开检查通过。HPU top 的结构检查在将两个 foundry SRAM 宏替换为临时接口黑盒后通过；真实宏行为模型含 Verilator 不支持的语法，未完成真实 SRAM 展开/仿真。这些检查不替代 VCS。
