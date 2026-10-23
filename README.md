# SteamLinkEyeInput

把外部 OSC 眼动数据送入 Steam Link 的原生注视点串流通道，提供 SteamVR 仪表盘与桌面调节工具。支持 PaperTracker 和可配置 OSC 地址的其他上游软件，使用原版 Steam Link 客户端。

**实验项目：实机基线为 SteamVR 2.18.4 / Quest 3。** 编码输入依赖 Valve 私有处理函数；结构发现减少了固定地址依赖，但不保证所有真实版本兼容。不是 Valve 或 PaperTracker 官方插件。

## 功能

- OpenVR 服务端加载项在 vrserver 内直接接收本机 OSC，不需要外部转发进程。
- 水平／垂直幅度 5–90°，左右／上下反转，持续眼追启停。
- 独立中心微调：左／右／上／下每次 0.5°，范围 ±20°，支持偏移归零。约 0.5 秒热加载，保留上游校准。
- SteamVR 仪表盘入口 **Eye Input**、桌面面板、托盘及 **Ctrl + Alt + E** 快捷键。
- **高清区画框 + 中心点**：保留原生画框，在双眼实际裁剪框中心绘制绿点，用户已确认居中且不会被控制面板遮挡。

## 工作方式和兼容性

上游校准后的归一化坐标通过 OSC 进入插件。插件应用幅度、方向和中心微调，生成以米表示的 HMD 相对目标点（+X 右、+Y 上、-Z 前），组装原生眼动包交给 Steam Link 的现有处理链，后者完成双眼投影与编码裁剪。过期或闭眼输入不会作为有效视线持续提交。

主路径通过消息特征、Windows 链式展开表、MSVC RTTI、接收／编码指令及构造字段关系交叉定位；不再写死接收函数 RVA、HMD 虚表 RVA、Provider 全局地址或使用的对象字段偏移。每帧经导出的 HmdDriverFactory 获取 Provider，核对 HMD 类型、所有权与会话。匹配缺失、歧义或冲突时停止私有调用，不猜测地址。

当前识别 MSVC x64 / D3D11 的代码形态。函数搬移后的内存副本测试通过，**不等于多个真实版本已经实测**。可选 OSC 菜单辅助仍依赖精确哈希，不是主路径提交的必要条件。当前已验证 Valve DLL SHA-256：

```text
8e7100b6b1e88d3ad6dd05f30f030dc9a69b092b64a60d35347cebb5c516b55d
```

中心点由电脑控制面板读取带序列号的共享内存裁剪快照，按每眼实际投影矩阵反算位置，以双眼纹理覆盖层绘制。0.35 米头部相对平面及前景排序防止面板遮挡；改距离时保持画面坐标和角尺寸。面板需保持运行，暂停、断线和数据过期时隐藏中心点。绿点不改变编码裁剪或眼追校准。

已删除未采用的官方输入后端、切换按钮、NDC 专用诊断和外置层／客户端改包实验工具。必要的结果与边界见 [未采用路线记录](docs/research-history.md)。公开 API 返回成功不能替代串流效果验收。

## 安装和使用

1. 从构建产物取得完整运行包，解压后运行 `安装插件.cmd`。
2. 安装目录使用纯英文路径。默认位置是 `%PUBLIC%\Documents\SteamLinkEyeInput`。
3. 启动或重启 SteamVR，并在 Quest 3 内连接 Steam Link。首次启动会注册仪表盘应用；之后通过 SteamVR 的启动应用机制加载。
4. 在头显内打开 SteamVR 仪表盘，选择 **Eye Input**；也可运行 `打开快速调节.cmd`、使用托盘图标或 Ctrl + Alt + E。
5. 在上游眼追软件中完成硬件和视线校准，并启用下面的 OSC 输出。

卸载时运行 `卸载插件.cmd`，并重启 SteamVR。仪表盘入口清理需要可用的 SteamVR 应用接口；若失败，启动 SteamVR 后重试。卸载脚本保留安装文件和上游软件配置。

从源码或发布包安装时，可显式指定位置：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Install.ps1 -SteamVR "C:\Program Files (x86)\Steam\steamapps\common\SteamVR" -InstallRoot "C:\VRTools\SteamLinkEyeInput"
```

## OSC 输入

接收地址为本机 `127.0.0.1:9100/UDP`，每个消息携带一个 OSC float 或 int。支持普通消息及 OSC bundle。

随附配置的四个必要坐标通道：

```text
/papertracker/EyeLeftX
/papertracker/EyeRightX
/papertracker/EyeLeftY
/papertracker/EyeRightY
```

可选的眼睑通道为 `/papertracker/EyeLidLeft` 和 `/papertracker/EyeLidRight`。没有眼睑数据时仍可工作。缺少四个必要坐标之一时不会生成完整注视点。

在 `steamlinkeyeinput/steamlinkeyeinput.ini` 中修改 `port` 和 `address_*` 可以适配其他软件。`profiles/osc-generic.ini` 提供 `/eyetracking/` 地址示例。输入是校准后的归一化水平／垂直标量，**不是像素坐标或直接以度表示的旋转角**。`yaw`、`pitch` 用于换算角度，`depth` 用于构造 Steam Link 接受的三维目标点；接近 90° 的目标角会限制在 89° 内以保持数值有限。

配置约每 0.5 秒重新读取。不要同时运行占用 UDP 9100 的旧桥接程序。若误改参数地址，可能出现“收到数据包但没有有效注视点”。


## 状态和限制

- status.json 提供结构匹配结果、发现的函数／虚表 RVA、收包和提交计数、数据年龄及异常状态。
- dashboard-status.json 提供覆盖层错误码及 center_dot_active。程序显示 active 不等同于视觉验收。
- 用户已确认当前环境的高清区跟随、中心微调及绿点居中／不被面板遮挡。
- data_age_ms 只表示插件收到数据后的年龄，不能当作端到端延迟。此前报告的跟随延迟尚未完成端到端归因。
- 标量到角度与固定深度仍是近似；边缘精度及其他真实版本需要另行验证。

## 构建和测试

需要 Windows x64、Visual Studio 2022 或 2019 C++ 桌面组件、CMake 3.20+。

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix dist/SteamLinkEyeInput
```

可用 `-DSTEAMLINK_DRIVER_TEST_FILE=本地driver_vrlink.dll路径` 启用只读真实 DLL 测试。测试不加载／执行厂商 DLL；覆盖函数 RVA 搬移、歧义候选、字段冲突和损坏 PE 拒绝。厂商文件不随项目分发，CI 不下载它。

其余测试覆盖 OSC、UDP、微调、闭眼／新鲜度、加载卸载、配置保留、双眼实际裁剪中心、不对称投影矩阵往返、前景平面坐标／角尺寸不变，以及真实只读共享内存并发一致性。本机启用真实 DLL 测试时共四项 CTest。

## 目录与许可

- src/driver.cpp：服务端接收和原生提交。
- src/native_layout.h：保守结构发现。
- src/center_dot.h：裁剪中心、几何及共享内存。
- src/control.cpp：桌面／仪表盘调节与绿点覆盖层。
- tests/：自动测试；tools/audit_vrlink_encoder.py：只读基线审计。
- package/：安装、卸载、默认配置及 OpenVR 运行库。

不包含厂商 APK、Valve 驱动 DLL、测试密钥、真实眼动记录或个人校准。项目代码 MIT；随附 OpenVR SDK 来自 [Valve 官方仓库](https://github.com/ValveSoftware/openvr)，遵循其 BSD 许可。
