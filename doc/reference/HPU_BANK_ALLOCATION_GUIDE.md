# HPU bank 分配与编译器调度

软件接入更新至 v1.2，2026-10-22；分配算法沿用 v1.1。配套 [64 对象软件编程手册](HPU_PROGRAMMING_MANUAL.md)。对应 HPU RTL 的确定性分配规则；**没有 PPLACE 指令**，OPC=9…F 保留。DLOAD mask 仅作用于本次装入。

同日追加的 [在线 PMUL 扩展](PMUL_ONLINE_COMPILER_HANDOFF.md) 将几何 pre/post
因子压缩为两条 line 参数，并特判其持续读成本；其他分配优先级和容量规则不变。

## 1. 同步规则能解决什么

同步分配规则后，编译器可以预测新结果放在哪里，并据此安排后续计算。前提是编译器维护真实对象生命周期、每个 bank 的区间占用和运行时配置；只有 `pID%5` 不能确定结果位置。

硬件负责本条结果分配时，优先避开本条读取的 bank。编译器负责初始源布局、既有目的复用、跨指令调度和容量预算。源对象已经同 bank 时，结果放置不能消除两个源之间的冲突。当前没有查询对象物理 base 的软件 ABI；不能靠读取一个未定义 CSR 修正预测。

## 2. 容量与空闲图

- p0…p63 是逻辑对象，ID 与 bank 无固定绑定；所有 bank 可有多个对象。
- Bank 0…4 各 1024 line，每 line 256 B。普通分配单元 128 line，每 bank 8 个单元。
- `units=next_pow2(ceil(actual_len/128))`。起始单元必须是 units 的整数倍，区间不能跨 bank。对象表保留 actual_len，空闲图保留 units。
- 每 bank 用 8-bit 占用图即可复现普通分配。实际长度 129 line 仍需两个单元，且起点只能为 0/2/4/6；分散的两个空闲单元不一定能用。
- Bank 5 共 32 line；默认前 4 line 永久保留模表，最多 64 contexts；余下 28 line 可放 14 个两条 line 参数对象。模表未装满也不能借用保留区。R 可配为 1…16，小对象起点需偶数对齐。
- 这 14 个槽由 NTT stage 与在线 PMUL pre/post 共同使用。10 份 pre/post 加模表占 24 line，只剩 4 个 stage 模板槽；生命周期由编译器统一安排。
- N8192 对象占一个普通单元，最多存储 40 个；执行 NTT/PMAC 需要同时保留旧对象和新结果，不能把 40 个全占满后期待自动原址执行。实际还要给向量常量留空间。

## 3. 分配契约

### 3.1 请求类型

| 请求 | 允许 bank | preference | 每 bank 的 read_cost |
|---|---|---|---|
| DLOAD 普通对象 | 本次 mask，默认 0…4 | `id%5` | 全 0 |
| PADD/PSUB 新目的 | 0…4 | `dst%5` | PSRC1、PSRC2 的读次数 |
| PMUL 向量/立即数新目的 | 0…4 | `dst%5` | PSRC1，加非立即数 PSRC2 |
| PMUL 在线因子新目的 | 0…4 | `dst%5` | 仅 PSRC1，参数不计持续读流 |
| PMAC 新结果 | 0…4 | `(旧dst_bank+1)%5` | 旧累加 dst、PSRC1，加非立即数 PSRC2 |
| NTT/INTT 新结果 | 0…4，排除输入 bank | `(input_bank+1)%5` | 输入 bank 两次读；它已被排除 |

输入 bank 为 5 时，NTT preference 取 0。正常多项式应放普通 bank。

同一个 bank、甚至同一个逻辑对象作为多个读操作数时，分别计数。例如 `pmac p0,p0,p0` 对 p0 所在 bank 的 cost=3；没有同址广播抵消。这是当前实际 stream 读请求模型。算术只写一条结果流，因此选择最少读次数的 bank 可减少结果写入带来的竞争；不代表能消除所有 SRAM 停顿。

在线 PMUL 每条指令只在启动时读两条参数 line。**参数即使位于普通 bank，也不计入
结果分配的持续 read_cost**。原址目的仍复用原位置；独立目的仍按 `dst%5`、
读成本、空闲区间和确定性次序分配，并占用普通容量。

DLOAD MOD=1 固定模表，mask 仅可 0/0x20；MOD=0 为普通对象，mask=0 默认普通 bank。stage 和在线 PMUL 参数也是 MOD=0，必须显式 mask=0x20 才仅选择 Bank 5，长度在使用时检查为 2。CP 是每条 DMA 独立的 LLC 分配提示，不改变本节的 SRAM 分配或释放规则。非零 mask 的 bit b 表示允许 Bank b；兼有 Bank 5 和普通 bank 时，先尝试 Bank 5 中合适的小区间，再尝试允许的普通 bank。结果分配不继承任何 DLOAD mask。

### 3.2 与 RTL 一致的选择伪代码

```python
# 正常成功执行路径。ownership 是当前已分配区间（含尚未提交的新结果）。
# 申请前等待所有在途 DMA 结束；计算按现有 stream 控制串行。
search = ownership.copy()
apply_completed_frees(search)
if atomic_replacement:        # DLOAD 重装或普通算术长度变化
    search.remove(old_dst)   # 只影响搜索视图，失败时保留旧对象

if request_allows_bank5:
    # MOD=1 只能从 line0 开始，长度 <= R，保留区不可被其他对象占用。
    # 其他小对象只在 [align_up(R,2),32) 取最低偶数起点连续区间。
    small = first_fit_bank5(search, actual_len, mod, R)
    if small is not None:
        return commit_allocation_atomically(small)

capacity = 128 * next_pow2(ceil(actual_len / 128))
best = None
for k in range(5):
    b = (preference + k) % 5
    if not allowed[b]:
        continue
    line = first_free_interval(search[b], size=capacity, alignment=capacity)
    if line is None:
        continue
    if best is None or read_cost[b] < best.cost:
        best = (b, line, read_cost[b])
    # 相同 cost 不覆盖：保留循环顺序中最早的 bank。

if best is None:
    fault_and_keep_old_object()
else:
    commit_allocation_atomically(best.bank, best.line)
```

普通 bank 的起点搜索严格递增。例如 preference=4 时，bank 顺序是 4、0、1、2、3。先选最小 cost，再以此顺序打破并列，不能发现第一个有空间的 bank 就停止。

普通算术/PMAC 的低冲突 bank 都满时，会选有空间且 cost 最低的剩余 bank。NTT 的输入 bank 排除是硬约束，不能放宽。所有允许 bank 都装不下时才失败；不静默 spill 到外部内存。

### 3.3 生命周期和预测时点

- DLOAD 每次分配；替换旧对象的空间仅在成功分配时回收。失败不改变旧 base/len/V。
- PADD/PSUB/PMUL 的既有目的实际长度匹配时不分配、不迁移，即使原 bank 有冲突。长度变化时，目的不能同时作为本次源来回收；合法软件应避免这种请求。
- PMAC 和每个 NTT/INTT stage 都申请新空间。发射后新旧两块均占用；exec_done 提交同一逻辑 ID 的新 base，之后回收旧块。编译器须更新映射，不能假定 NTT 永远在固定两 bank 之间交替。
- PFREE 对象不忙时回收。DSTORE REL=0 保留；REL=1 仅成功完成时回收，发射时还不能使用该空间。
- 任意新分配都等此前 DMA 完成，再采纳释放和分配。独立既有目的算术可与 DMA 重叠，但不改变区间所有权。这使布局不依赖 DMA 快慢。
- 算法预测依赖无 fault 的有序执行。遇到 fault 应停止依赖该结果的路径，不能沿用“本条必成功”的预测继续运行。

编译器可在内部按程序顺序模拟正常完成后的所有权；不需要模拟 DMA 的周期延迟。运行时控制流、动态长度或不同 R 会改变布局，应按路径/参数重新推导，或选用容量确定的调度模板。

## 4. 调度建议与示例

### 分散源，再预测结果

例如将 p0 通过 DLOAD mask=0x01 放 Bank 0，p1 通过 mask=0x02 放 Bank 1。`padd p5,p0,p1` 的 preference=0，若 Bank 2…4 可用，则结果选 Bank 2。`padd p5,p5,p1` 再次使用 p5 时复用 Bank 2，硬件不会主动换 bank。

PMAC 旧累加位于 Bank 0、两个乘数位于 Bank 1/2 时，有空间的 Bank 3/4 优先。其中 preference=1，所以两者都有空间时选 Bank 3。若 Bank 3/4 都满、0/1/2 有空间，就在这三个 bank 中选最低读 cost，并以 1、2、3、4、0 的顺序打破并列。

源同 bank 时，分开它们是软件的工作。可在初始 DLOAD 时固定不同 bank；也可使用新逻辑目的做计算并更新软件映射。必要时 `pmul pNew,pOld,1` 可复制 canonical 系数，但需额外读写、计算时间和存储，不能当成免费的迁移。不要对已有匹配长度目的期待一次普通算术自动搬迁。

### 把新逻辑 ID 当作并列偏好

新 PADD/PSUB/PMUL 目的使用 `dst%5` 打破并列。编译器可选一个空闲 ID，使预期 bank 在循环顺序中靠前，但这只是并列偏好，不会越过读 cost 和容量规则。PMAC/NTT preference 由旧物理 bank 决定，与目的 ID 无关。

### 为 NTT 留跨 bank 工作区

输入 Bank 0 时，NTT 优先 Bank 1，有空间则迁移到 1。下一 stage 优先 Bank 2；若 2…4 满而 0 有空间，则返回 0。因此能否来回使用两块空间取决于占用图；不能仅以“还剩一个普通单元”推断任意 stage 都可执行，该单元还必须位于允许 bank。

Bank 5 的参数对象在每条计算指令开头只读两条 line；递推状态保存在内部寄存器，
不写回参数对象。默认 4-line 模表加 N8192 一方向 13 个 stage 参数占 30/32 line，
此时只能再放一份在线 pre/post 模板，不能把两套容量预算相加。

stage 模板按 `B=N/128` 分解蝶形因子；在线 PMUL 按 **`B=N/64`** 分解
`c*r^j`。后者 post 的首元素是 `N^-1`，不使用要求 U[0]=1 的 stage 校验。
参数可跨多项式复用，使用期间必须保持有效；下一条在线 PMUL 自动重置递推状态。
本项目为两种 `kind=twiddle` 参数的 DLOAD 统一设置 CP=1，其余 DLOAD 和全部
DSTORE 的 CP=0；CP 不影响 SRAM read_cost，也不表示参数必然命中 LLC。

### DMA 与计算重叠

可先发射独立 DMA，再发射使用已存在目的的 PADD/PSUB/PMUL，尽量让 DMA bank 与算术读写 bank 分离。新目的算术和 PMAC 要申请空间，必须等 DMA 结束；NTT 也等待 DMA。当前 stream 控制不支持任意先后顺序都能对称重叠。

## 5. 冲突与验证边界

每个物理 bank 单周期只能完成一次读或写。不同对象、不同 line 在同 bank 仍冲突。普通二源算术有 2R+1W，PMUL 立即数有 1R+1W；在线 PMUL 的持续数据流是 1R+1W，启动时另外读取两条参数 line，因子更新复用内部 PE。PMAC 对象模式有 3R+1W，立即数有 2R+1W。NTT 的数据通路是同输入 bank 的 2R、同输出 bank 的 2W，输入输出分开也有固有串行访问。

本策略降低当前结果写入与读取的竞争，未引入同址读取广播、双口 SRAM 或全程序最优布局求解。编译器仍应以目标 RTL 的仲裁/流水和容量仿真评估吞吐；上述访问数不是端到端周期数保证。

2026-10-21 提供方记录的控制器小测试覆盖容量、高位 ID、OPC9 非法、PMAC 第二源避让、不同 cost 选择、只有冲突 bank 可用时的回退、既有目的复用、新分配等待 DMA。未验证真实 SRAM/PE 数据吞吐或 CPU/SoC IT；RTL 更新只在 HPU 设计目录生效。

上述局部记录没有覆盖 v1.2 的 CP 属性传递。CPU 命令通路与实际 DMA 的 ARCACHE/AWCACHE 仍须按新版手册单独验收；不能由 allocator 回归推断 LLC 性能。
