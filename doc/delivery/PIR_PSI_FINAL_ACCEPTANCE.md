# PIR/PSI 本轮验收摘要

基线 `7afc64ec3245c0fdba5a8bb8512cd744ba6680e2`，分支 `feature/trident-pir-psi-hardware-readiness`。状态只表示本轮对应层级：PASS=实际通过，FAIL=实际失败，BLOCKED=缺接口/环境，NOT RUN=本轮未执行，HISTORICAL=仅旧证据。

| 验收项 | 状态 | 依据与边界 |
|---|---|---|
| PSI 重复输入/异常输入 | PASS | 自定义输入按 raw item 去重并在多项式构造前生效；专门测试通过。 |
| 差分脚本空跑、错 `--full`、子进程异常 | PASS | 3 个失败路径测试均确认非零退出和摘要。 |
| SenderDB 原生预处理差分 | PASS | 13/13；`../readiness-senderdb-differential.log`。 |
| SenderDB 完整 HPU 软件差分 | PASS | 本轮 `single` 1/1，13 个案例均完成预处理；`../readiness-senderdb-full-single/summary.json`。旧 6 例完整差分仅 HISTORICAL。 |
| 小规模 PIR 边界与 PSI 应用 | PASS | `../readiness-app-ctest.log` 13/13；8/9 相邻明文边界见 `../readiness-boundary-ctest.log`；最终 PIR 代码另见 `../readiness-pir-final-ctest.log` 6/6。 |
| PIR 合成读回适配器 | PASS | 能从包 span 导入、BR 转换、分解后作为阶段二输入，错误镜像拒绝；`../readiness-readback-adapter.log`。数据来自 Golden，**不算独立执行结果**。 |
| 从软件执行器最终内存独立导出并完成两阶段联动 | BLOCKED | 现有公开接口不提供执行后内存；需原作者只读导出接口。 |
| 目标侧不重编译更换第二阶段输入 | NOT RUN | 当前应用重新编译；动态镜像重装/指令稳定性未建 ABI。 |
| 既有正式包格式 | PASS | 原作者 `hpu_validate_package` 19/19 返回 0；[逐包结果](PIR_PSI_PACKAGE_VALIDATION.tsv)。是**静态校验**。 |
| 正式 PIR/PSI 本轮重建及运行 | NOT RUN | 应用默认图/输入未改；既有产物仅静态审计，生产来源仍是旧 dirty 提交。 |
| 旧正式规模软件结果 | HISTORICAL | `finalization-pir-target`、`finalization-psi-target` 历史验证，不冒充本轮重跑。 |
| Nexus-AM 导入、ELF/BIN、RTL/SoC/真实 HPU | NOT RUN | 固定接收器无 PIR/PSI case；未修改 Nexus-AM。 |

进入 Nexus-AM **适配开发阶段**所需的包格式、逐包 manifest、数值 Golden 和小规模软件回归已具备；进入正式硬件验收仍须先完成下列工作：

1. HPU 软件执行器维护者提供与 `CompiledApplication` 本次编译绑定的最终 HPU_MEM 只读导出；用真实导出文件运行 `--stage0-readback`，验证 8/9 边界及目标记录，保存 SHA-256。硬件接入时换为实际 DSTORE/DDR 读回。
2. 应用方在干净、固定提交重建正式 2+17 包，重新运行原作者校验器，保存包 SHA-256 与生产 commit；现有 `dirty-at-configure` 产物只作审计参考。
3. Nexus-AM/IT 团队按[硬件就绪报告](PIR_PSI_HARDWARE_READINESS.md)加入小规模 06 组 C 壳和清单，再加 PIR 两阶段 CPU 主控与 PSI 17 包调度；构建 ELF/BIN 并逐 word 自检。
4. 硬件团队确认实际 window、DDR、缓冲和 64 对象/NTT v4/在线 PMUL/IRQ 版本，尤其 PIR stage0 的 516,261,120 B 镜像；确认后按六级顺序执行 RTL/SoC 测试。

本轮未合并长期分支，未重写历史，未修改上游或 Nexus-AM。
