# OptiScaler 神经网络渲染（DLSS-NR）简体中文汉化版

把 [janblade/OptiScaler-F5-DLSSNR-Multipass](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass)
的游戏内界面汉化成简体中文的版本。

原版是一个用 NVIDIA AI 改变游戏**光照、细节和颜色**的画面 mod —— 你可以自己调效果的强度、
观感，以及它吃掉多少性能。本仓库只做界面汉化，不动任何功能。

> 这是社区实验性分支的汉化，效果和游戏兼容性因游戏而异。

**[下载最新版](https://github.com/Owiilnice/OptiScaler-dlssnr-F5-ZH/releases/latest)** ·
[上游项目](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass) ·
[安装说明（上游，英文）](INSTALL-DLSSNR.md) ·
[维护说明](MAINTAINING.md)

## 这个汉化版做了什么

- **游戏内叠加菜单全部简体中文**（按 `Insert` 打开的那套界面），当前 1200 余条词条
- **界面中文字体随包附带**（文泉驿微米黑），不用自己去系统里挑字体
- **功能零改动**：所有设置项、默认值、渲染路径与原版完全一致，只是文字变中文
- 少数诊断信息**刻意保留英文**：FPS 读数、`nvngx_dlss` / `libxess` / FSR 版本行、Vulkan 驱动行。
  这些是给你对着日志和网上资料排错用的，翻成中文反而搜不到

汉化不是「把中文写进源码」，而是一套可以反复套用的词典 + 结构补丁。上游更新后重新套一遍就行，
**不会因为上游改了代码就失效**。想了解怎么做的，看[维护说明](MAINTAINING.md)。

## 你需要什么

- **NVIDIA RTX 20 / 30 / 40 / 50 系显卡**，和一款支持 64 位的游戏。老卡会明显更慢
- **另需下载模型文件 `nvngx_dlssnr.dll`** —— 它不在发布包里（NVIDIA 的授权限制）。
  用哪个版本取决于你的显卡，见[上游安装说明](INSTALL-DLSSNR.md#choose-the-correct-runtime)

## 安装（Windows）

1. 关掉游戏，备份已有的 mod 文件
2. 下载[最新版发布包](https://github.com/Owiilnice/OptiScaler-dlssnr-F5-ZH/releases/latest)，
   把**所有文件**解压到游戏 exe 旁边
3. 把上面那个模型文件放进同一个文件夹
4. 运行 `setup_windows.bat`，问到显卡时选 **NVIDIA**
5. 进游戏选 DLSS，按 **Insert** 打开菜单 → 启用 Neural Rendering，先跑一遍（1 pass）

## 注意

- 神经网络渲染**有性能开销**，也可能出现闪烁等画面问题。开在 Ray Reconstruction 之前仍是实验性的
- **混合模式（hybrid）是给 RTX 50 用的**，文件已包含。加载时游戏会卡住几秒，看起来像死机，等它一下
- **别在带反作弊的联机游戏里用**
- 帧生成（Frame Generation）相关设置见[上游说明](docs/DLSS-FRAME-GENERATION.md)

## 出问题了

先看 `OptiScaler.log`（和 `OptiScaler.dll` 在同一目录）。

- **功能本身的 bug** → 提到[上游仓库](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/issues)，
  带上游戏、显卡、设置和日志
- **汉化文本的问题**（漏译、错译、中文显示成方块）→ 提到[本仓库](https://github.com/Owiilnice/OptiScaler-dlssnr-F5-ZH/issues)

## 发布包内容

包名形如 `OptiScaler-ZH-<版本>.7z`：

| 内容 | 说明 |
| --- | --- |
| `OptiScaler.dll` | 主程序，已汉化 |
| `nvngx.dll_dlssnr.dll` | 转发器 |
| `OptiScaler/` | XeSS、FidelityFX、DirectX 运行库 |
| `font/wqy-microhei.ttc` | 界面中文字体 |
| `OptiScaler.ini`、`setup_windows.bat`、`setup_linux.sh` | 配置与安装脚本 |
| `SHA256SUMS.txt` | 文件校验和，可用 `sha256sum -c` 验证 |
| `README.md`、`INSTALL-DLSSNR.md`、`LICENSE`、`docs/` | 文档与许可 |

> **关于体积**：包约 54 MB。这不是「比上游少了东西」—— 上游那个 126 MB 的包用的是
> ZIP/Deflate，我们用 7z LZMA2 压同样的内容，两者原始体积都是 212 MB 左右。
> 详见[维护说明](MAINTAINING.md#发布包内容)。

## 致谢与许可

本汉化建立在以下项目之上，功劳都是他们的：

- [OptiScaler](https://github.com/optiscaler/OptiScaler)
- [Dagherbou 的 Neural Rendering 分支](https://github.com/Dagherbou/OptiScaler_DLSSNR)
- [RenoDX](https://github.com/clshortfuse/renodx)（色彩处理）
- 本汉化跟随的上游分支：[janblade/OptiScaler-F5-DLSSNR-Multipass](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass)

界面字体为[文泉驿微米黑](https://www.wenquanyi.org/)，以 Apache License 2.0 及
GPLv3（含字体嵌入例外）发布。

OptiScaler 本身的许可见包内 `LICENSE`，完整致谢见 `docs/CREDITS.md`。
