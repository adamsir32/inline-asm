# 编译器适配说明：stage 内在线 twiddle（K=8）

> 历史说明：本文记录迁移时的旧 ISA。当前对象号、编码、DLOAD TYPE/mask 与 REL 行为以 [64 对象 v1.1 手册](../reference/HPU_PROGRAMMING_MANUAL.md) 为准；旧机器码不能直接复用。K=8 参数数学布局继续适用。
日期：2026-10-21。交付对象：编译器的常量生成、HPU lowering、对象管理、数据镜像和 DMA relocation 维护者。

## 1. 需要完成的修改

**保留 PNTT/PINTT 指令编码，将每个 stage 的完整 twiddle 表改为 512 B 参数对象，再更新该对象的 DMA 长度、布局和绑定信息。**

当前 RTL 的所有 PNTT/PINTT 默认在线生成，没有完整表执行分支。旧机器码可以复用，旧 twiddle 数据镜像不能直接复用。这是 **src2 操作数格式更新**，不是指令字段扩展。

| 编译器环节 | 修改要求 |
|---|---|
| Target 配置 | 固定匹配硬件 `TWIDDLE_CHAINS=8`；建议在编译产物元数据中记录参数格式版本和 K，便于区分旧数据镜像。这些元数据不放入硬件参数 line。 |
| 常量生成 | 为每个 `(N,q,root,direction,stage,physical_layout,K)` 生成两条参数 line。第一阶段可直接压缩已有的正确完整表。 |
| 数据镜像/常量池 | 每 stage payload 固定 512 B，按 256 B line 对齐；重新计算后续常量偏移、镜像总长度与校验值。 |
| DMA relocation/binding | stage 参数 DLOAD 的 `line_count=2`；`line_offset` 指向重排后的参数块。同步更新 spans、binding、manifest、外存窗口大小等描述。 |
| 对象长度/合法性检查 | src2 为参数对象，其逻辑长度是 2 line；多项式 src1 的长度仍为 `N/64` line。不能再按“src2 长度=N/128”检查 NTT。 |
| 指令生成 | PNTT/PINTT 的 opcode、dst、src1、src2 对象 ID、stage、MODE2 等原编码保持不变；不增加软件 GEN/PMUL 指令。 |
| 生命周期 | 参数经普通对象 DLOAD 后供对应 stage 使用；完成前不得释放或覆盖。已有每 stage `DLOAD → PNTT/PINTT → PFREE` 的依赖关系继续保留。 |
| 测试/打包 | 更新二进制常量及引用它的所有 sidecar；先完整重构 twiddle 验证，再做 stage/最终结果对拍。 |

本次 IT 用例的实际迁移没有改生成的 `ntt.c`，全部 57 条 HPU 指令字保持不变，只替换数据镜像和调用者传入的 spans。指令源文件 SHA-256 为：

```text
8880bd37cd7c4c186c3dd3458877a6cada0c84eeaabfaf62cf7602b2ffdfe500
```

对应已推送 RTL：IT-SCPU-RTL `new/2bdbf7c`；Nsnhuv_5.0_integra `main/2f4d8b39b`。下述 ABI 以这两个提交中的默认 K=8 为准。

## 2. 硬件组与 twiddle 的物理顺序

设多项式长度 `N=2^L`，当前支持 `128 <= N <= 65536`。每个硬件组处理 128 个数据系数、64 个蝶形，因此每个 stage 有：

```text
B = N / 128                  # 硬件组数，必须为正的二次幂
T_s[b, ell]                  # 组 b 的第 ell 个 BF 实际使用的因子
0 <= b < B, 0 <= ell < 64
T_s_flat[64*b + ell] = T_s[b, ell]
```

`T_s_flat` 必须按**真实 BF 消费顺序**排列。前向在本 stage 的 loader 取数后、输出 P 置换前取因子；逆向在输入 P⁻¹ 置换后取因子。不能把自然逻辑索引顺序的表直接当作该物理表，也不能只因数学指数正确就省略 loader/置换顺序核对。

PNTT 与 PINTT 都使用各自原来的 stage 编号，参数必须跟相应的那条 stage 指令一致。PINTT 使用当前实现的 lazy-scaled BF 因子，不应简单用前向表逐项取逆或倒序来代替。已有的正确完整表/模型是迁移基线。

K=8 表示 **8 条交错的 stride-8 因子链**，不是新指令字段，也不改变 64 个 PE 的宽度。RTL 第 b 组使用 slot `b % 8`，消费后更新该 slot，供第 `b+8` 组使用。

## 3. 两条参数 line 的精确格式

一条 line 为 256 B，即 64 个 `uint32_t`。一个 stage 固定 128 个 word、512 B。按 IT 数据镜像的 **little-endian u32** 序列化；第 ell 个 word 对应 RTL `line[ell*32 +: 32]`。数值使用普通模 q 余数表示，与既有 BF twiddle 一致，不改成 Montgomery 表示。

以下 word 索引都从 0 开始：

| line | line 内 word | 对象内字节偏移 | 含义 |
|---|---:|---:|---|
| 0 | `0..63` | `0..252`，步长 4 | `U[0..63]`：第 0 个硬件组的 lane 模板 |
| 1 | `0..6` | `256..280`，步长 4 | `V[1]..V[7]`：slot 1～7 的初始化倍率 |
| 1 | `7+t` | `284+4*t` | `R[t]`：stride-8 的第 t 级二进制进位倍率 |
| 1 | 其余 | 对应 word 偏移 | 填 `1` |

`V[0]=1` 隐含，不占 word。**不要额外写入 q、N、stage、K、header 或长度字段。** 模数及 Barrett 上下文仍由原 PMODLD 路径提供。

有效 R 数量为：

```text
r_count = max(0, log2(B) - 3)
```

例如 N=4096 时 B=32，只有 `R[0]、R[1]`，分别位于 line 1 的 word 7、8；N=65536 时 B=512，有 `R[0..5]`。B<8 时，未用到的 V 填 1，所有 R 填 1。

**小 N 也保持 K=8 的固定布局。** 不能因为 B<8 就把 R 起始位置从 word 7 改成其他位置；也不能恢复一条 line 或完整表模式。即使 N=128，仍发两条参数 line。

## 4. 从已有完整表生成参数：推荐的第一版实现

对本次 IT 使用的标准根表，有 `T_s[0,0]=1`。定义：

```text
U[ell] = T_s[0, ell]
V[a]   = T_s[a, 0]                          # 1 <= a < min(8,B)
R[t]   = T_s[8*2^t, 0] / T_s[8*(2^t-1), 0] (mod q)
```

这里的除法是模逆乘法，不是整数除法。R 只对 `8*2^t < B` 的 t 生成。

这些参数必须满足以下重构关系。将 `b=a+8*z`：

```text
C[a, ell] 初始为 U[ell] * V[a] mod q，V[0]=1
第 b 组输出 C[b % 8, ell]
若 b+8 < B：
    t = ctz((b >> 3) + 1)
    C[b % 8, ell] = C[b % 8, ell] * R[t] mod q
```

倍率为 1 时 RTL 自动跳过内部 PMUL；stage 末尾不会更新不再使用的 slot。编译器不需要发射这些更新，也不需要按 PE 的 14 拍反馈延迟生成软件调度。

下面是可直接用于迁移校验的 Python 3.8+ 参考实现，仅依赖标准库。输入是一个 stage 已有的正确物理表，输出就是可写入镜像的 512 B：

```python
import struct


def pack_stage_twiddle(table, q, n, chains=8):
    if not (128 <= n <= 65536 and n & (n - 1) == 0):
        raise ValueError("unsupported N")
    if chains != 8:
        raise ValueError("this target ABI requires K=8")
    if len(table) != n // 2:
        raise ValueError("expected one physical twiddle per BF")
    if table[0] != 1 or any(not (0 < x < q) for x in table):
        raise ValueError("expected canonical nonzero roots, T[0]=1")

    batches = n // 128
    u = list(table[:64])
    meta = [1] * 64
    for a in range(1, min(chains, batches)):
        meta[a - 1] = table[64 * a]

    r_count = max(0, batches.bit_length() - 1 - 3)
    for t in range(r_count):
        before = chains * ((1 << t) - 1)
        after = chains * (1 << t)
        meta[chains - 1 + t] = (
            table[64 * after] * pow(table[64 * before], -1, q)
        ) % q

    # Mandatory migration check: verify every reconstructed BF factor.
    state = []
    for a in range(chains):
        v = 1 if a == 0 else meta[a - 1]
        state.append([x * v % q for x in u])
    for b in range(batches):
        a = b % chains
        if state[a] != list(table[64 * b:64 * (b + 1)]):
            raise ValueError("table does not match this physical recurrence")
        if b + chains < batches:
            z_next = b // chains + 1
            t = (z_next & -z_next).bit_length() - 1
            ratio = meta[chains - 1 + t]
            state[a] = [x * ratio % q for x in state[a]]

    return struct.pack("<128I", *(u + meta))
```

IT 适配脚本中的 `compress()` 使用相同的 U/V/R 提取和逐元素重构验证。该脚本针对已知素数 q，模逆用 `pow(x,q-2,q)`；上面使用 `pow(x,-1,q)`，会在逆元不存在时直接失败。

重构不一致时应停止打包，检查表顺序、方向、lazy scale、root 和参数格式。不要静默回退到旧表，当前 RTL 没有旧表路径。若编译器此前把归一化或其他常数折入 stage 因子，不能直接套用 `T[0]=1` 的这段提取代码；本次迁移保留原外置 scale/twist，并以重构和 BF 对拍确认等价。

## 5. 后续可直接生成小参数，不必先生成完整表

第一版可以保留完整表生成器作为离线 oracle，再压缩到输出镜像。之后若希望降低编译期内存，当前物理指数满足：

```text
p = 64*b + ell
E_s(p) = sum(p_r * c[s,r]) mod N
T_s_flat[p] = omega ^ E_s(p) mod q
```

其中 `c[s,r] = E_s(1<<r)`，必须来自当前方向、当前 loader/置换/lazy scale 的物理指数。由此可直接生成：

```text
U[ell] = omega ^ E_s(ell) mod q
V[a]   = omega ^ E_s(64*a) mod q
R[t]   = omega ^ (c[s,9+t] - sum(c[s,r] for r in range(9,9+t))) mod q
```

R 指数先模 N，空和为 0。这个 R 是 stride-8 进位倍率，不能替换成相邻组的固定倍率。前向与逆向的物理指数实现见本地研究脚本 `tools/research/twiddle_optimization_probe.py` 的 `exponent()`，以及 `generate_vectors.py` 中的直接参数生成；第一版后端不必依赖该可选优化。

每个 RNS 模数、root、方向、stage 生成自己的数值参数；K 和物理排列约定也应进入常量缓存键。参数每条 stage 指令重新加载，不复用上一 stage 的内部缓存状态。本次没有跨 stage 更新。

## 6. 指令与对象管理的具体要求

### 6.1 PNTT/PINTT 编码

不扩展 opcode，不设置在线 enable 位，不占用 MODE2。保留原来的 stage 编号、dst/src 对象 ID 和 MODE2=0。以交付用例为例：

```text
pntt p3,p0,p1,stage=0 : 0x4600405B，保持不变
pntt p0,p3,p1,stage=1 : 0x40C0445B，保持不变
参数 dload x10,x11,p1,TYPE=1,FLAG=0 : 0x02B5202B，保持不变
```

DLOAD 的长度通过 x11 的寄存器值传递，因此将其值从 32 改为 2，不需要修改这条指令的机器码。PNTT 中 src2 仍然是 p1 对象 ID，不是参数地址或长度。

上述十六进制字对应 IT 交付 CPU 的译码约定，不是跨 SoC 强制编码模板。另一个 SoC 后端应保留其已有、与 CPU 译码匹配的 custom opcode 和 HPU payload 编码，不要因 twiddle 格式迁移而替换 opcode；上游 README 的 custom opcode 说明也应按该目标单独核对。

### 6.2 参数仍为普通对象

参数 DLOAD 保持原普通对象类型，例如上述 `TYPE=1,FLAG=0`。不要因 payload 变成 512 B 就自动改为模数表/小 bank 加载。模数对象和 PMODLD 的布局、mu 等原上下文保持不变。

正确的逻辑长度关系为：

```text
src1 polynomial: N/64 line
src2 parameters: 2 line
dst polynomial: N/64 line
hardware batches: (N/64)/2 = N/128
```

N/B 从多项式形状推导，不再从 src2 常量的长度推导。硬件依赖检查要求 src2 已分配、有效、不忙，长度至少两条 line；编译器本格式统一生成恰好两条。

普通对象的物理分配仍可能预留半 bank。常量 payload/传输量减少，不代表编译器可以假定只占两条普通 SRAM 物理 line；物理分配仍遵循目标 allocator 的现有规则。

### 6.3 PINTT stage 0 必须加载参数

旧逻辑可能因逆向 stage 0 因子全为 1 而省略 src2。当前 RTL 不保留该特殊对象旁路：**PINTT stage 0 也必须有有效的两条参数 line**。对当前全 1 stage，U、V、R 全部填 1；硬件会跳过倍率为 1 的 GEN 运算。

### 6.4 其他向量保持原语义

显式 pre-twist/post-twist、归一化、普通 PMUL 操作数不属于这两条 stage 参数 line。交付 IT 的 pre-twist 是 N 个 word，仍加载 64 line，供 PMUL 使用。不能把所有名称含 `twiddle` 的文件一概截为 512 B。

## 7. 实际 IT case 的完整前后布局

用例：N=4096、q=50061313、12 个前向 stage，negacyclic 的 pre-twist 显式执行。以下偏移和长度均以 **256 B line** 为单位，相对于 HPU 外存窗口。

| DMA span | 用途 | 旧 `(offset,count)` | 新 `(offset,count)` |
|---:|---|---|---|
| 0 | 模数上下文 | `(129,1)` | `(129,1)` |
| 1 | 输入多项式 | `(1,64)` | `(1,64)` |
| 2 | pre-twist，供 PMUL 使用 | `(130,64)` | `(130,64)` |
| 3 | stage 0 参数 | `(194,32)` | `(194,2)` |
| 4 | stage 1 参数 | `(226,32)` | `(196,2)` |
| 5 | stage 2 参数 | `(258,32)` | `(198,2)` |
| 6 | stage 3 参数 | `(290,32)` | `(200,2)` |
| 7 | stage 4 参数 | `(322,32)` | `(202,2)` |
| 8 | stage 5 参数 | `(354,32)` | `(204,2)` |
| 9 | stage 6 参数 | `(386,32)` | `(206,2)` |
| 10 | stage 7 参数 | `(418,32)` | `(208,2)` |
| 11 | stage 8 参数 | `(450,32)` | `(210,2)` |
| 12 | stage 9 参数 | `(482,32)` | `(212,2)` |
| 13 | stage 10 参数 | `(514,32)` | `(214,2)` |
| 14 | stage 11 参数 | `(546,32)` | `(216,2)` |
| 15 | 输出 DSTORE | `(65,64)` | `(65,64)` |

对本例，新 stage s 参数的位置为 `194+2*s` line。其他项目应由自己的常量布局器计算位置，不应硬编码这些 IT 偏移。

实际打包过程：保留原镜像前 194 line，接上 12 个 512 B 参数块，最后保留原最后一条 guard line。因此：

| 项目 | 旧 | 新 |
|---|---:|---:|
| 每 stage 根表/参数 payload | 8192 B | 512 B |
| 12 stage payload | 98304 B | 6144 B |
| DLOAD 总长度（包括 mod/input/pre-twist） | 513 line | 153 line |
| DSTORE 长度 | 64 line | 64 line |
| 此 IT 镜像/配置窗口总长度（含保留区和 guard） | 640 line / 163840 B | 219 line / 56064 B |

镜像总长度与 DLOAD 长度之和是不同口径。若像本例一样缩短镜像，HPU_MEM 窗口大小应更新为 **219 line**，不是 153 line；所有 span 都必须落在窗口内。若后端选择保留原 padding/窗口大小也可以，但 payload 和对应 DLOAD 长度必须更新。

`spans[3+s]` 只需更新成新 offset 和 count。当前交付 `ntt.c` 从 spans 读取 x10/x11，并更新自身对象长度记录，所以迁移时无需修改函数体。若其他后端将这些值固化为常量或 shape 校验，应同步修改其生成逻辑。

### 7.1 数值检查点

下面只对应这份交付用例的 stage 11（同 q 但选择不同本原根时不要求相同）：

```text
U[0..7] = 1, 2564634, 41945451, 26788067,
          15836256, 8007847, 43637878, 16593746
U[63]   = 37489420
V[1..7] = 44649554, 3474171, 962682, 31511628,
          25687718, 1581086, 23227686
R[0]    = 35622094
R[1]    = 35622094
```

将全部 U、7 个 V、2 个 R 和剩余填 1 的 word 按 `<128I` 序列化，512 B 块的 SHA-256 为：

```text
d210dc6952f25f6a2407583320f741c8d8db3eda37ce17f0ed7a58e7457db835
```

本例 stage 0 的整个参数块为 128 个 u32 的 1，SHA-256：

```text
d585e10d1e2240e9af79fc1cf8d11e11420b5306480b469b587e85630fcb0c9f
```

这些检查点可以定位 word 顺序、K=8 元数据偏移和大小端打包错误。

## 8. 编译器交付的验收要求

1. **数据格式**：每个方向、stage、RNS basis 的参数对象恰好 512 B，line 对齐；所有有效根/倍率是正确的 u32 模余数。
2. **完整重构**：按第 4 节递推重构 N/2 个 BF 因子，与迁移前正确物理表逐项一致。包括 B<8、B=8、B>8，以及 R 进位切换。
3. **指令编码**：已有基线的 PNTT/PINTT 指令字不变，MODE2 不充当在线开关；IT 示例全部 57 条 HPU 指令字不变。PINTT stage 0 原先若省略参数 DLOAD，现在需补回加载/对象依赖。
4. **布局/长度**：新 spans、binding、constant manifest、window length、对象长度记录一致；验证 `offset+count <= window_lines`；普通多项式、pre/post twist 和输出大小不被缩短。
5. **功能**：逐 stage BF 因子及输出对拍；前向/逆向完整结果、原 golden 和 guard 检查通过。逆向保持现有 lazy scale/归一化约定，不另改数学结果。
6. **上下文**：不复位地切换方向、q、root 和 stage 时参数正确重载；不将不同上下文的参数错误去重共用。
7. **运行时**：真实 SRAM 下 stage 参数读请求每次两条 line。N=65536 每方向 16 个 stage 合计 32 line / 8 KiB；不再按每个 BF 组读完整表。

现有 RTL 控制链回归覆盖 N=128/256/1024/4096/65536、NTT/INTT、q=2013265921/998244353/4293918721、背压与在途复位。编译器首先完成 N=4096 交付用例，再以这些尺寸/模数覆盖参数和布局边界；特别用 q>2^31 的配置检查无符号处理。

### 8.1 已知硬件合流器问题：与参数迁移分开验收

未经修复的 HpuCmdMerge 会在背压时让 custom0 PNTT 越过较早的 custom1 DLOAD，导致 stage 3 在队头等待其后面的参数加载。本次完整 IT GOOD TRAP 使用了**本地生成 SV 验证补丁**，但按用户要求，该合流器补丁没有提交到 IT 仓库；上游的 Chisel 合流器本次也未修改。

因此，仅拉取上面两个 twiddle 提交，不能据此承诺未节流 IT 已消除这个死锁。正式集成需修复 Chisel 侧的提交顺序，并确认重新生成后仍正确。迁移编译器时不要把该硬件越序归因为 twiddle 参数算法，也不要用永久增加每 stage 软件等待作为本次格式的强制 ABI。`build_it.py --pace-dma` 仅是已验证的诊断绕过方式。

详细波形和 Chisel 修复要求见 [合流器反馈文档](HPU_CMD_MERGE_CHISEL_FEEDBACK.md)。对应上游源码为 `Nsnhuv_5.0_integra/src/main/scala/linknan/hpu/HpuCmdMerge.scala`。

## 9. 实际参考文件与重跑入口

以下路径均相对 IT-SCPU-RTL 根目录，位于本次工作区；此前只推送了 RTL 与构建清单，不能假定这些本地测试/文档文件都已进入远程提交。

| 文件 | 参考用途 |
|---|---|
| `ver/it/core/Difftest/tests/twiddle/soc/build_it.py` | 最接近编译器迁移的实现：`compress()`、stage 镜像重排、spans 更新、原指令字检查 |
| `ver/it/core/Difftest/tc/nexus-am-hpu-workloads/provenance/transform-data/ntt/` | 原交付 ntt.c、delivery.h、完整表镜像与 golden |
| `ver/it/core/Difftest/tests/twiddle/soc/results/20261021/manifest.json` | 新的完整 spans、512 B payload 统计、源文件一致性及未节流标记 |
| `ver/it/core/Difftest/tests/twiddle/generate_vectors.py` | 直接由物理指数生成 U/V/R，含 NTT 和实际 lazy INTT |
| `rtl/cpu/SCPU_RTL/RTL/hpu/rtl/control/hpu_twiddle_stream.sv` | 硬件对参数 word 的实际解释与 stride-8 更新 |

生成改造后的 IT 数据和 ELF：

```sh
python3.12 ver/it/core/Difftest/tests/twiddle/soc/build_it.py \
  --output /tmp/hpu-twiddle-compiler-check
```

该入口默认不加 DMA 软件等待。输出目录中的 `online/ntt.c`、`online/ntt_execute.c` 均与原交付 C 文件相同；`online/window.bin`、`online/it_data.h` 与 `manifest.json` 是编译器同事应重点对照的产物。

控制链真实 SRAM 验证与完整 IT 的条件、结果分别见 [RTL/IT 验证报告](TWIDDLE_SRAM_IT_VALIDATION.md) 和测试目录 README。完整 IT 默认重跑入口为：

```sh
bash ver/it/core/Difftest/tests/twiddle/soc/run_it.sh
```

使用该入口验证未节流程序前，先落实第 8.1 节的合流器修复。CPU 全量核对交付 golden 的通过标记为 `HIT GOOD TRAP`，脚本汇总标记为 `TWIDDLE_IT_PASS`。


## 10. 本文参考代码的核对结果

已直接执行第 4 节代码，确认：

- 交付 IT 的全部 12 个 stage，512 B 输出与实际 IT 适配脚本逐字节一致；stage 0/11 的数值哈希、全部 spans 和 56064 B 镜像长度一致。
- 已有 7 组 N/q/root 配置的前后向共 162 个 stage，累计 2,208,640 个物理 BF 因子全部重构一致；U、有效 V/R 与另一条物理指数生成路径一致。

这是对本说明中的生成算法和迁移示例的核对，不代表编译器后端已经完成适配。后端仍应按第 8 节提交自己的数据产物、指令/布局检查和 RTL 对拍结果。
