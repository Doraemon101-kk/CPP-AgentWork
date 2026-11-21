# 修复演示摘要（已脱敏）

此文件是实际运行后的人工整理摘要，不是逐字原始日志。已移除本机绝对路径、会话ID、提案ID和耗时；源码来自仓库自带样例。原始会话与日志不随发布包分发。

模型：`qwen3:4b-instruct-2507-q4_K_M`。

```text
用户：根据刚才的测试失败修复问题，请生成实际修改提案。
模型：查看项目地图、读取测试和calculator.cpp、搜索add。
C++：返回实际文件内容和行号。
模型：调用propose_edit，生成待应用提案。
```

预览的实际变更：

```diff
- return left - right; // BUG: addition should add both inputs
+ return left + right; // Fixed: addition of both inputs
```

实际检查序列：

| 阶段 | CTest结果 |
|---|---|
| 原始代码 | 失败：add必须执行加法 |
| 用户应用模型提案后 | 通过 |
| 用户撤销提案后 | 再次失败；原始文件逐字节恢复 |

这个例子验证一条修复链路，不表示所有任务或模型都能成功。

v0.2再次用真实模型验收同类链路，编辑任务使用`/edit`。模型生成的注释措辞可以不同，以实际提案和CTest结果为准。
