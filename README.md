# CppPilot · v0.2.0 实验版

<img src="assets/icon-pixel.png" width="96" height="96" alt="CppPilot 图标">

一个使用本地 Ollama 模型的 **Windows C++ 项目工作台**：查代码、持续对话、生成修改提案、预览/应用/撤销、执行真实 CMake/CTest、导出报告。

本项目以 vibe coding 方式开发，主要实现由 AI 生成和迭代，发起者尚未逐行审阅全部代码。请阅读[AI 开发说明](AI_DISCLOSURE.md)。本版本适合自己的学习与实验项目。

## 环境准备

- Windows 10/11 x64，PowerShell 5.1 或 7。
- [CMake](https://cmake.org/download/) 3.26+，加入 PATH。
- C++17 编译器：MSVC 2022 的“使用 C++ 的桌面开发”组件，或 MinGW-w64 GCC 9+；新安装建议使用维护中的工具链。
- [Python](https://www.python.org/downloads/) 3.9+（运行测试；无 pip 依赖）。
- [Ollama for Windows](https://ollama.com/download/windows)。默认模型 `qwen3:4b-instruct-2507-q4_K_M`，下载约2.5GB；建议16GB及以上内存。首次下载需要联网。

源码包含 JSON 库及许可证；模型、Ollama、编译器不随源码包分发。

## 从零构建与启动

在项目根目录打开 PowerShell。编译工具已在 PATH 或开发者终端中时：

```powershell
.\build.ps1
ctest --test-dir build -C Release --output-on-failure
.\start-model.ps1
.\pilot.ps1
```

MSVC 可显式选择生成器：

```powershell
.\build.ps1 -Generator "Visual Studio 17 2022"
```

MinGW 不在 PATH 时，用 `-Compiler` 指定 **你自己电脑上** 的 g++：

```powershell
.\build.ps1 -Compiler (Join-Path $env:USERPROFILE "tools/mingw64/bin/g++.exe")
```

输出统一在 `build/bin/cpp_agent.exe`。自定义构建目录时，后续启动传同一个 `-BuildDir`。只构建程序可用 `-NoTests`，此时不要求 Python，也不宣称测试已执行。

Ollama 已经运行时直接复用；否则启动脚本从 PATH、标准安装位置或 `-OllamaPath` 找到程序。默认连接本机 `127.0.0.1:11434`。自定义端口要同时传给启动和工作台：

```powershell
.\start-model.ps1 -Port 11435
.\pilot.ps1 -Port 11435
```

可用 `-ModelsDir` 指定已下载模型目录；脚本不依赖项目外的固定目录。如需跳过脚本，也可使用下方原生命令。

## 完整体验

默认打开内置故障项目，按顺序操作：

1. `/test`：编译并运行测试，看到加法测试失败。
2. 输入 `/edit 根据测试失败修复问题，请生成实际修改提案`。
3. `/changes`、`/review ID`：查看实际提案。
4. `/apply ID`、`/test`：应用后验证测试通过。
5. `/undo ID`、`/test`：恢复原始代码，测试重新失败。
6. `/report`：导出本次会话与检查结果。

模型只生成提案，应用由用户命令触发。失败的修改请求会标记为未完成。
打开自己的项目：

```powershell
.\pilot.ps1 -Root ../my-cpp-project
```

## 常用功能

| 用途 | 命令 |
|---|---|
| 单次模式 | `/ask 问题`、`/edit 修改任务`、`/check 检查任务` |
| 默认模式 | `/mode ask`、`/mode edit`、`/mode check`；新会话默认ask |
| 长任务记忆 | `/note 目标和约束`、`/memory`、`/compact` |
| 取消 | 活动任务中按Ctrl+C；另一终端同项目可执行`/cancel` |
| 文件/函数/TODO | `/map`、`/symbols main`、`/todos` |
| 读取与搜索 | `/read calculator.cpp`、`/search add` |
| 构建与测试 | `/configure`、`/build`、`/test` |
| 修改管理 | `/changes`、`/review ID`、`/apply ID`、`/undo ID`、`/discard ID` |
| 会话 | `/sessions`、`/resume ID`、`/new`、`/history` |
| 配置与报告 | `/config`、`/model NAME`、`/report`、`/verbose` |
| 退出 | `/quit` |

状态默认保存在 `.agent_data/`，按项目隔离。默认只读，提到“修复”不会自动要求改码；`/edit`才开放提案工具，`/check`开放构建测试但禁用提案。模式在程序层过滤工具。恢复上下文包括近期最多4轮完成问答、较早问答的摘要、固定任务笔记和源码版本事实。当前任务保留成对工具请求与结果。模型通过本机 HTTP 处理请求，首轮下载完成后不需要云端模型服务。

## 工程结构

```text
src/app/          会话、命令、Agent循环
src/workspace/    索引、路径/存储、修改管理
src/runtime/      HTTP客户端、进程执行
include/          模块接口
cmake/            配置时生成本机工具链信息
fixtures/         无私人内容的样例
examples/         人工脱敏后的演示摘要
scripts/          启动辅助与源码打包
tests/           确定性回归、可选真实模型演示
docs/            架构、开源参考、发布说明
third_party/     JSON库及原许可证
```

## 验证与发布资料

- 确定性测试不要求 Ollama 或模型，验证真实文件操作、构建、测试、会话和修改管理。
- 可选真实模型演示：`python tests/run_live_demo.py`；自定义服务可传 `--port` / `--model`。
- 原生命令：`build/bin/cpp_agent.exe --workbench fixtures/repair_project --port 11434`。
- [脱敏演示摘要](examples/repair-demo.md)、[发布验证](docs/发布验证.md)、[发布说明](docs/发布说明.md)。

## v0.2：模式、记忆与流式取消

- `/ask`、`/edit`、`/check`只作用于本次任务，`/mode`切换后续任务的默认模式。
- 文字逐段显示为`[draft stream]`，完整消息通过检查后才标记完成。工具参数收全、校验后才执行；中断的流不会执行半个工具调用。
- Ctrl+C取消当前模型请求或检查进程树，工作台返回输入状态；中断保存为`interrupted`。已应用的修改不会被取消操作擅自撤销。
- 如从另一终端取消，使用同一Root和StateDir：`./pilot.ps1 -Root ../my-cpp-project -Execute /cancel`。不需要进入原会话。
- `/note`固定任务目标与约束，`/note -`清空。`/memory`查看摘要和事实，`/compact`整理较早对话。
- 摘要由模型生成并标为未核实；失败时使用明确标记的摘录回退，原始问答仍保留。文件SHA-256变化后，旧读取及检查记录标记为非当前证据；检查版本覆盖已索引源码，不覆盖外部依赖环境。
- 上下文预算采用保守字节估算，不是精确tokenizer；超限时缩短工具输出并整体移除旧消息组，仍超限会报错。

## 已知限制

Windows 单平台；默认小模型仍可能选错工具或提出错误代码。函数/TODO索引为词法候选，不是 Clang 语义分析。只支持现有目录中的文本提案，单文件上限256KiB，多文件修改逐项应用。旧 MinGW 对非ASCII项目路径构建不可靠，建议英文路径。CMake/CTest会执行选定项目的构建逻辑，仅用于自己信任的本地项目。路径检查不提供抵抗恶意并发目录替换的系统级隔离。

本机已验证 MinGW 路径，MSVC 的构建和测试配置提供于 Windows CI；远端 CI 是否通过以 GitHub 实际运行结果为准。发布验证文档明确区分干净副本、真实模型和远端 CI。

## 许可证与参考

原始代码：[MIT](LICENSE)。依赖及原许可证见[第三方声明](THIRD_PARTY_NOTICES.md)。采用的开源设计见[开源参考](docs/开源参考.md)。
