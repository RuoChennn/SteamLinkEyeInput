# SteamLinkEyeInput

把外部 OSC 眼动数据送入 Steam Link 的原生注视点串流通道，并提供 SteamVR 仪表盘与桌面快速调节工具。

**实验项目，目前仅适配一个已验证的 SteamVR / Steam Link 驱动版本及 Quest 3。** 输入协议可配置，不依赖某个眼追硬件品牌；随附 PaperTracker 的 OSC 地址示例。它不是 Valve 或 PaperTracker 官方插件。

## 功能

- OpenVR 服务端加载项随 SteamVR 启动，在 `vrserver.exe` 内接收本机 OSC 数据。
- 自动显示 Steam Link 的 OSC 选项，不更改眼追共享许可或原有 OSC 输出设置。
- 眼动不经过外部转发进程；外部调节工具只修改配置、显示状态。
- 头显内的 SteamVR 仪表盘入口：**Eye Input / 眼追调节**。
- 桌面面板、托盘入口与全局快捷键 **Ctrl + Alt + E**；关闭窗口收起到托盘。
- 水平、垂直幅度调节范围 **5–90°**；方向开关；持续眼追开关。
- 校准由 PaperTracker 或其他上游软件负责，不保存独立回中偏移。

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

## 兼容性和已知限制

已验证的 `driver_vrlink.dll`：SteamVR **2.18.4**，SHA-256：

```text
8e7100b6b1e88d3ad6dd05f30f030dc9a69b092b64a60d35347cebb5c516b55d
```

本项目使用官方 OpenVR 加载和仪表盘 API，但将眼动接入编码器的部分依赖该版本的 Valve 私有布局与处理函数。校验不匹配时停止私有调用；SteamVR 更新后可能需要重新适配。当前只向 Quest 3 内部型号提交数据。

端口 9100 是**本插件新增的接收入口**。Valve 原有的 OSC 通道不能据此视为通用眼追输入端口。数据进入原生编码处理并不意味着具备 Quest Pro 的完整硬件时序或精度。

- 已验证：真实眼动进入原生处理函数，双眼编码裁剪随之变化；仪表盘入口和点击可用。
- 仪表盘采用持久 D3D11 纹理；用户已确认替换原始位图刷新后闪烁消失。
- 用户仍报告约 **0.5 秒**的高清区跟随延迟，尚未完成端到端归因或修复。不能把状态中的 `data_age_ms` 当作端到端延迟。
- 中央准确但边缘有误差的问题尚未完全解决。当前标量到角度的换算仍是近似，固定深度也可能影响双眼投影。
- 90° 上限需要加载新版服务端 DLL；更新后应重启 SteamVR。数值有限性已通过本地测试，最终头显体验仍需验收。

## 构建和测试

需要 Windows x64、Visual Studio 2022 或 2019 的 C++ 桌面开发组件，以及 CMake 3.20 或更高版本。

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix dist/SteamLinkEyeInput
```

GitHub Actions 在 Windows 上执行同样的配置、构建、测试和运行包生成。工作流上传构建产物，不自动创建公开 Release。

测试覆盖 OSC 解析、自定义地址、上游校准直通、方向、闭眼、过期数据、90° 数值保护、实际本机 UDP 接收、加载／卸载以及面板修改配置时保留其他字段。测试不等于硬件精度或端到端延迟测量。

## 目录

```text
src/driver.cpp       SteamVR 服务端加载项
src/gaze.h           OSC 解析与坐标换算
src/control.cpp      桌面／托盘／SteamVR 仪表盘调节工具
src/vendor/          OpenVR SDK 头文件与导入库
package/             安装脚本、清单、默认配置及 OpenVR 运行库
tests/               本机自动测试
.github/workflows/   Windows 自动构建
```

不包含厂商程序、Valve 的 `driver_vrlink.dll`、实际眼动记录、设备校准或个人配置。随附 OpenVR SDK 文件来自 [Valve 官方仓库](https://github.com/ValveSoftware/openvr)，遵循其 BSD 许可；项目代码遵循 MIT 许可。
