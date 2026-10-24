# 保持 RTL 不变的 NTT/INTT 软件布局适配方案

日期：2026-10-24。针对 `two_core_no_fdi@e7a1d04`、现有 K=8 在线因子生成器与 TC `fd0db077` 的 N=1024、L4 用例。

## 1. 核实结论

**可以继续在线生成预乘因子。** 位逆序后的 twist 因子虽然不再是按相邻物理 word 等比的序列，仍可由现有 `U[64] + V[1..7] + R[ctz]` 格式精确表达，只占原来的两条 256-byte 参数 line，不需要修改 RTL、PE、stream 地址流或新增指令。

软件需要同时调整：

1. NTT 的 DLOAD 输入改为系数位逆序布局。
2. 预乘因子按相同索引位逆序后，重新打包成两行 K=8 参数。
3. INTT 后乘因子也按 raw 输出布局重打包；结果在软件接口边界恢复自然序，或在内部统一保留位逆序系数布局。

**不能仅换预乘因子而完全不调整输入布局。** 标量逐点乘法不能代替任意系数置换；这与因子能否在线生成是两个不同问题。

已对当前四个 RNS 模数进行两行参数展开、NTT/INTT、往返、乘以 X、稠密多项式乘法的离线检查，全部零误差。尚未修改实际 case、重编 ELF 或进行修正后的 VCS 仿真；不是 RTL 回归 PASS 声明。

## 2. 保持什么不变，改变什么

保持当前执行顺序：

```text
NTT : DLOAD -> 在线预乘 PMUL -> PNTT stage0...log2(N)-1 -> DSTORE
INTT: DLOAD -> PINTT stage0...log2(N)-1 -> 在线后乘 PMUL -> DSTORE
```

不改变各 stage 的现有因子参数、stage 顺序、指令编码、PMUL mode=1、参数 DLOAD 长度=2 line、小 bank mask=32、普通多项式长度=N/64 line。对象槽、释放关系、DMA cache policy 及多 outstanding 模型均无需因本方案调整。

设 `N=2^d`，`BR_d(j)` 是 **d-bit 索引位逆序**。它不反转 uint32 数值的比特，也不打乱 RNS 模数分量顺序。

| 数据域 | 本方案物理存放方式 |
|---|---|
| HPU 输入/中间系数域对象 | `physical[j] = natural[BR_d(j)]` |
| NTT 域对象 | 保留原完整变换 golden 定义的 HPU canonical physical 布局 |
| 对外暴露的明文/系数数组 | 软件导入/导出时转换，仍可维持自然序 API |

不能对所有 buffer 无差别位逆序：系数域与 NTT 域必须有不同的布局标记。

## 3. 为什么输入重排可以前移，而 RTL 预乘顺序不变

令自然序系数为 `a[i]`，原自然序预乘因子为 `p[i]=psi^i mod q`。

之前离线定位中，与 golden 相符的是“先自然序预乘，再 BR，再进入 raw PNTT”。可以等价改写为：

```text
x[j] = a[BR_d(j)]
p_hw[j] = p[BR_d(j)] = psi^BR_d(j) mod q
x[j] * p_hw[j] = (a * p)[BR_d(j)] mod q
```

所以只需在软件准备 DLOAD 数据时重排系数，并同步改变预乘参数，硬件仍然执行“PMUL 在前、PNTT 在后”。不需要在两个硬件命令之间插入重排过程。

对 N=1024，物理位置 0、1、2、3 的输入改为自然系数 0、512、256、768；对应预乘指数也是 0、512、256、768。

只有常量表重排、没有数据重排时，上式不成立。不得通过依赖具体明文数值的因子去补偿，那不能形成通用变换，遇到零系数还可能无法定义。

## 4. 现有在线生成器为什么能表达这些预乘因子

每个 PMUL batch 消费一条 64-word 数据 line。定义：

```text
B = N/64 = 2^m
j = 64*b + ell          # ell=0..63，b=0..B-1
BR_d(j) = B*BR_6(ell) + BR_m(b)
F[b,ell] = psi^(B*BR_6(ell)) * psi^BR_m(b) mod q
```

这正好分解为“64 个 lane 共用同一批间标量”，符合现有 RTL 的广播倍率更新能力，并不要求相邻物理 word 保持恒定公比。

### B >= 8 时的直接构造

令 `b=8*z+s`，`s=0..7`，`h=m-3`：

```text
U[ell] = psi^(B*BR_6(ell)) mod q
V[0]   = 1                          # RTL 隐含，不存
V[s]   = psi^(2^h * BR_3(s)) mod q  # s=1..7
```

同一条 chain 从 batch `b` 更新至 `b+8` 时，`z` 增加 1。设 `t=ctz(z+1)`，位逆序指数的变化为：

```text
BR_h(z+1)-BR_h(z) = 3*2^(h-1-t)-2^h
R[t] = psi^((3*2^(h-1-t)-2^h) mod (2*N)) mod q
```

负指数先对 psi 的阶 `2*N` 取模，不能用无符号整型直接承接负值。`h=0` 时没有 R 更新。

这与现有 RTL 的 `next_z=(batch_q>>3)+1`、`ctz(next_z)` 和 `lanes[slot] *= R[t]` 相同。相关实现：`rtl/cpu/SCPU_RTL/RTL/latest/hpu_twiddle_stream.sv`，尤其 next_z/ctz 选择、参数读取及更新逻辑。

### 当前 N=1024 的具体参数

此时 `B=16,m=4,h=1`：

```text
U[ell] = psi^(16*BR_6(ell))
V[1..7] 的指数 = [8,4,12,2,10,6,14]
R[0] = psi
其余未使用参数 = 1
```

例如 `U[1]=psi^512`；第 8 个 batch 相对第 0 个 batch 乘 psi，正好对应 `BR_10(512)=1`。

当前 q0 的 `q=134072321,psi=58811`；其他模数必须各自使用其对应 psi，不得复用 q0 常量。

### B < 8 时

N=128/256 对应 B=2/4。仍使用上述 U；有效 slot 的 `V[s]=psi^BR_m(s)`，无 R 更新，未用 V 与 padding 填 1。

已对 N=128、256、512、1024、2048、4096、8192、16384、32768、65536 全部做预乘参数展开检查。最大 N=65536 只需要 7 个 R，仍满足两行参数容量。此为格式/数学检查，不代表这些尺寸已做过当前 SoC 仿真。

## 5. 两行参数格式与软件打包要求

```text
line0: U[0..63]
line1: V[1..7], R[0..r_count-1], padding=1
```

每项为 little-endian uint32。按 `params[64+s-1]` 写 V，按 `params[71+t]` 写 R；V[0] 不占位置。PMUL 的 B=N/64，不可误用 NTT stage 的 B=N/128。

如果现有软件接口只有 `pack_online_pmul_parameters(c, ratio, q, N)`、只能构造自然序 `c*ratio^i`，需要增加 layout-aware 参数构造或通用 exact packer，**不能只换 c/ratio 两个常量**。硬件格式支持，不等于现有高层打包接口已经支持。

通用打包方法，适用于预乘和当前实测可压缩的后乘序列 F：

```python
# 前提：len(F)=N，N/64 为 2 的幂，F 的所有值在 (0,q) 内
params = F[:64] + [1] * 64
for s in range(1, min(8, N//64)):
    params[64+s-1] = F[64*s] * inverse_mod(F[0], q) % q
for t in range(max(0, log2(N//64)-3)):
    before = 512*((1 << t)-1)
    after  = 512*(1 << t)
    params[71+t] = F[after] * inverse_mod(F[before], q) % q
assert expand_with_rtl_recurrence(params, N//64, q) == F
```

最后的全量展开断言不可省略：任意向量不一定能压成该格式，不允许只验证每条 line 的第一个 word。也不要直接复用要求 T[0]=1、完整表长度 N/2 的 stage-twiddle packer；PMUL 后乘允许 F[0] 不等于 1，长度应为 N。

## 6. INTT 后乘与自然序接口

设当前生成器的**完整自然序后乘向量**为 `post[i]`，它包括对应 raw PINTT 实现所需的 lazy-scale 修正、N 逆及 inverse twist。

```text
post_hw[j] = post[BR_d(j)]
raw_PINTT_output[j] * post_hw[j] = a[BR_d(j)] mod q
```

对 raw 输出使用这个重排后的因子，再由软件把输出转换为自然序：

```python
# src/dst 不重叠；BR 是自逆置换
for i in range(N):
    natural_output[i] = hpu_output[BR_d(i)]
```

当前 N=1024 四个模数的 `post_hw` 均通过两行 K=8 exact packing 与全量展开检查，仍能在线生成。对未来其他尺寸/参数，应对实际生成的 post 向量执行上述打包断言，不把“预乘任意支持尺寸已证明”外推成“所有未知 post 表必然可压缩”。

不要把 post 草率替换成 `N^-1*psi^-i`：当前逆蝶形还涉及 lazy-scale。应从现有完整正确 post 向量出发做索引适配，保留其数学含义。

如果需求是“HPU 内部 INTT 结果本身必须在自然物理地址，且不允许 CPU/软件边界重排”，本方案**不满足这个额外条件**。因子乘法本身不能搬移系数；此时必须另行提供置换实现或重新设计整套调度/表约定，不能宣称只改两行参数即可。

## 7. 软件侧具体修改清单

### 7.1 先修 04 组独立 NTT/INTT 用例

- NTT：数学输入 a 与独立 SEAL golden 保留；仅把实际 DMA 输入打包为 `a[BR(j)]`，预乘参数替换为 `pack(p[BR(j)])`。各 stage 因子参数原样保留。
- INTT：NTT 域 DMA 输入及原数学 golden 保留；把后乘参数替换为 `pack(post[BR(j)])`。检查结果时解码 BR，或以 `physical[j]` 对 `golden[BR(j)]` 检查，明确这是布局适配而非放宽比较。
- 输入只读区/guard 的参考镜像必须同步实际 DMA 输入布局；不得仍按改动前自然序镜像检查。
- 每个模数分量分别做长度 N 的 BR。不能对拼接后的 L*N 元素整体做 BR，否则会串 RNS 分量。
- 参数仍是两行；若布局/offset 变化，重新生成完整 DMA span 绑定、image、BIN/ELF 和 manifest，避免 ASM、payload、元数据不一致。

源码排查入口（以实际构建 pin 的版本落实，不使用过期工作区替代）：

- `tests/hputest/tools/group_cases/composite.cpp::transform()`：输入、输出、pre/post 资源绑定。
- 上游 `CkksApplicationImageBuilder::add_canonical_twiddles()` 及对应 BFV/BGV 的表构造：布局参数化，不宜盲目全局改表。
- 上游在线 PMUL 参数打包器：支持重排后的非“物理相邻等比”向量，并展开校验。
- 软件指令执行模型：回归当前 raw-stage 契约，不能一边显式把数据改成 BR，一边仍在 stage0/末级隐式再次 BR，造成双重转换。

### 7.2 扩展到完整 FHE 程序时

这是一次系数域存储 ABI 调整，不能只修独立 case 而假定 KeySwitch 自动正确：

- HPU 内部系数域对象统一为 BR 布局；NTT 域保持所约定的 canonical physical 布局。
- 加减、逐系数缩放以及跨 RNS 的逐系数基转换，所有输入/常量必须对齐同一逻辑系数。
- 自同构/Galois 等系数置换要显式共轭地址映射：若自然序是读取 `src[g(i)]`，BR 物理序应读取 `src_phys[BR(g(BR(j)))]`；符号变化及完整模 2N 映射仍按原算法执行。
- 预编码明文、常量缓存、scratch 布局和 CPU 参考检查都需标明域及布局。不要把已经在 BR 系数域的缓冲区再次重排。
- 若中间对象始终留在 HPU，INTT->逐系数操作->NTT 可以沿用 BR 系数对象，不必每次为自然序 API 做 DDR/CPU 往返；但具体算子资源/常量必须逐项复核，不能据此宣布所有 FHE 算子已适配。

## 8. 验证结果及验收要求

本次从官方 NTT/INTT ELF 提取实际输入、golden 和因子，使用已由波形核对的 raw-stage 算法检查，不修改 golden 的数学值。

| 离线检查 | 覆盖与结果 |
|---|---|
| 重排预乘向量 -> 两行参数 -> RTL 递推展开 | 四分量各1024字，零差异 |
| 重排后乘向量 -> 两行参数 -> RTL 递推展开 | 四分量各1024字，零差异 |
| NTT 输出 vs 原 NTT golden | 四分量4096字，零差异 |
| INTT 解码为自然序 vs 原 INTT golden | 四分量4096字，零差异 |
| NTT -> INTT 往返 | 四分量，零差异 |
| NTT(a) .* NTT(X) -> INTT vs 独立环移位/取负 | 四分量，零差异 |
| 两个稠密多项式变换域乘法 vs 独立 O(N²) 模 X^N+1 卷积 | 四分量，零差异 |
| N=128..65536 的重排预乘参数展开 | 10种尺寸，零差异；不是 SoC 回归 |

后续实际验收仍需：发布适配后的官方 ELF，在当前多 outstanding 模型上依次跑 04 NTT/INTT、NTT->PMUL->INTT 的独立环乘法用例，再扩展 KeySwitch。不能只用 NTT->INTT 往返作为验收，因两端不一致可能相互抵消。

复算命令：

```bash
cd /eda/users/xuxicheng/IT-SCPU-RTL
/usr/bin/python3.12 tools/research/verify_ntt_software_layout_20261024.py
```

详细结果（含四个模数的完整128-word预乘/后乘参数）：

[software_layout_adaptation.json](/eda/users/xuxicheng/IT-SCPU-RTL/ver/it/core/Difftest/sim/regress/delta0304_tc39959486312_multi16_20261024/wave/04_03_intt_l4_n1024/traceweave_debug/software_layout_adaptation.json)

## 9. 可直接转给软件同事的任务描述

在不改变当前 RTL stage/PE/在线生成器和 NTT、INTT 指令顺序的前提下，建立显式 raw-stage 布局契约：NTT 的系数 DMA 输入采用 BR_N；预乘 `psi^i` 先按 BR_N 重新索引，再生成现有两行 K8 参数。INTT 的完整 post 因子同样按 BR_N 索引并 exact-pack，输出按 BR_N 解码回自然序。每个 RNS 分量独立处理，NTT 域物理布局保持原 canonical golden 定义，各 stage 参数不改。在线 PMUL packing 必须全向量展开验证，不能仅支持 c*r^i 的现有便捷接口，也不能遗漏 post 的 lazy-scale 修正。模型不能在已有显式 BR 布局上重复做隐式边界 BR。保留独立数学 golden，并以 NTT、INTT、往返、NTT->PMUL->INTT 对独立环卷积共同验收。此方案已做离线验证，仍需官方构建及 VCS 确认。
