# APSI PSI HPU 底层零改动审核报告

## 审核范围

- PSI 前基准：`8b3cbbff4155cb5f5ac0567f98d4acb4657c3fc7`
- 被重构提交：`632c734fb87d40a37c32c6af7ff310407bd17005`
- 重构结果：本报告所在的 `feature/operator-resource-management` HEAD
- 禁止修改目录：`src/hpu/`、`src/scheme/`、`include/hpu/`、`include/scheme/`

`632c734` 曾为 APSI 修改 9 个底层文件，内容包括显式 BFV 明文模数接口，以及
三分量 BFV Add/Sub 的 frontend、plan、codegen、relocation、software executor 和
汇编生成支持。本次重构逐项撤销这些修改，没有重置仓库，也没有撤销
`8b3cbbf` 或更早版本已有的任何功能。

同时恢复了只为上述底层扩展增加的两个通用单元测试：
`test/seal_bfv_context_test.cpp` 和 `test/seal_evaluator_test.cpp`。

## 可复现审核命令

```bash
git diff --exit-code \
  8b3cbbff4155cb5f5ac0567f98d4acb4657c3fc7..HEAD -- \
  src/hpu src/scheme include/hpu include/scheme

git diff --name-status \
  8b3cbbff4155cb5f5ac0567f98d4acb4657c3fc7..HEAD -- \
  src/hpu src/scheme include/hpu include/scheme \
  test/seal_bfv_context_test.cpp test/seal_evaluator_test.cpp
```

两条命令均应无输出，第一条退出码应为 0。

## 逐文件 Git blob 审核

下表左、右 SHA 分别来自：

```bash
git rev-parse 8b3cbbff4155cb5f5ac0567f98d4acb4657c3fc7:<path>
git rev-parse HEAD:<path>
```

| 文件 | `8b3cbbf` blob | 重构后 blob | 一致 |
| --- | --- | --- | --- |
| `include/hpu/seal/bfv_context.hpp` | `3409edf72497f076d5fc187c2bf74a70948a97be` | `3409edf72497f076d5fc187c2bf74a70948a97be` | 是 |
| `include/scheme/bfv/basic_arithmetic.hpp` | `562d8ec3fa636c1f710b67b8d1aca68b9063e6bc` | `562d8ec3fa636c1f710b67b8d1aca68b9063e6bc` | 是 |
| `src/hpu/seal/bfv_context.cpp` | `4539643035d1ab3831782ac4b11fab9e611f467d` | `4539643035d1ab3831782ac4b11fab9e611f467d` | 是 |
| `src/hpu/seal/bfv_operation_codegen.cpp` | `46f2f611a08f076bc445bb90924d65539bd5649e` | `46f2f611a08f076bc445bb90924d65539bd5649e` | 是 |
| `src/hpu/seal/bfv_operation_plan.cpp` | `5b3ee30385bcea0035af91645cc41bcd4111e420` | `5b3ee30385bcea0035af91645cc41bcd4111e420` | 是 |
| `src/hpu/seal/bfv_operation_relocation.cpp` | `2b3357d39e12b27a4031b2b7425e1393464fa714` | `2b3357d39e12b27a4031b2b7425e1393464fa714` | 是 |
| `src/hpu/seal/bfv_software_executor.cpp` | `68bb3f570023b03dc6444a2ae705894fa6c4335d` | `68bb3f570023b03dc6444a2ae705894fa6c4335d` | 是 |
| `src/hpu/seal/evaluator.cpp` | `c9ac1272653b4911ba2151169616cf5ab6ef00ae` | `c9ac1272653b4911ba2151169616cf5ab6ef00ae` | 是 |
| `src/scheme/bfv/basic_arithmetic.cpp` | `beb9ad2423c1616a2354f24f14199d437f958707` | `beb9ad2423c1616a2354f24f14199d437f958707` | 是 |
| `test/seal_bfv_context_test.cpp` | `37e76c30248e4e8df0f5050819b1d87dcd6415cd` | `37e76c30248e4e8df0f5050819b1d87dcd6415cd` | 是 |
| `test/seal_evaluator_test.cpp` | `0eba64b5fc4e875d8bde0e382e9cd9e191acaf42` | `0eba64b5fc4e875d8bde0e382e9cd9e191acaf42` | 是 |

## 重构后的允许范围

相对 `8b3cbbf`，PSI 交付只保留应用入口、应用级测试、CMake 注册、README 和本文档。
精确 `t` 由应用使用 SEAL 公共 API 构造上下文；Paterson–Stockmeyer 的每个三分量
乘积立即调用既有 `relinearize`，后续只使用原有两分量 Add。没有新增 HPU 算子、
Evaluator 接口、OperationPlan 类型、Codegen、Relocation 或软件执行路径。
