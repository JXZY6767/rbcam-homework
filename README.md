# 工业相机取流与 YOLO 装甲板识别作业

C++17 项目，使用海康 MVS SDK 与 OpenCV 完成 USB 相机连接、连续取流、曝光/增益设置、像素格式转换、YOLO 推理、检测结果显示，以及缓冲区归还和异常退出清理。

本仓库包含源码、可运行的ONNX模型、原始图片/CSV、运行日志、实验分析和演示素材。**当前归档尚未满足全部作业要求：真实机器人图片均为静止，运动曝光对比未采集；已保存的连续识别录像是工业相机拍摄手机回放，直接拍真实装甲板的动态演示待补。** 固件和部分现场环境信息也未记录。

## 作业材料入口

| 内容 | 入口 |
| --- | --- |
| 六项实验结果、分析与局限 | [实验报告](docs/lab_record.md) |
| 16张图片的参数及灰度/饱和统计 | [逐图索引](docs/capture_index.md) |
| BayerRG8 / BGR8性能对比 | [格式性能记录](docs/format_performance.md) |
| 浅/深拷贝与异常清理 | [内存及错误处理记录](docs/memory_error_handling.md) |
| 手机回放正常/低置信度对照 | [补充对照](docs/phone_replay_comparison.md) |
| 手机回放该帧未检出案例 | [案例记录](docs/phone_replay_no_detection.md) |
| 连续识别补充录屏 | [工业相机拍手机回放，约16.6秒](docs/evidence/replay_demo_trimmed.webm) |
| 作业要求与证据映射、缺项 | [提交清单](docs/submission_checklist.md) |
| 程序主要步骤 | [代码说明](docs/code_walkthrough.md) |

## 环境与依赖

- Ubuntu 26.04.1 LTS，x86_64；GCC 15.2.0，C++17；命令行CMake 4.2.3。
- OpenCV 4.10.0，包含 core、imgproc、imgcodecs、highgui、dnn、videoio。
- MVS安装在 `/opt/MVS`；使用其头文件和64位 `libMvCameraControl.so`。
- 安装包发行说明版本：MVS 5.1.1 Build20261015，Camera SDK 4.9.0.3 Build20261015。
- 实测USB相机：MV-CS016-10UC，序列号DA4302122，图像1440×1080。CPU推理使用OpenCV DNN。
- MVS SDK须在运行机器上自行安装，仓库不附带SDK库。运行C++程序不需要启动Python或MVS客户端；运行前释放其他客户端占用的相机。

模型已包含在 `models/best.onnx`，来源、形状和导出环境见 [模型说明](models/README.md)。未要求训练新模型，未实现串口、跟踪和弹道解算。

## 编译

在解压/克隆后的项目根目录打开终端：

```bash
cmake -S . -B build
cmake --build build -j 4
./build/work --help
```

CLion用户直接打开项目，重新加载CMake，选择 `work`。运行参数填在“运行 → 编辑配置 → work → 程序实参”，只填写下面命令中 `./build/work` 后面的部分。

CMake默认按 `/opt/MVS/include` 与 `/opt/MVS/lib/64/libMvCameraControl.so` 查找SDK。SDK安装在其他位置时须修改CMake对应路径。当前程序只枚举USB相机，不支持直接连接GigE设备。

## 运行与按键

真实相机取流与识别：

```bash
./build/work --serial DA4302122 --exposure 1000 --gain 0
```

其他相机将序列号换成SDK枚举输出中的实际值。无参数时代码默认选择原先调试的虚拟设备 `Vir53692095`，因此真实相机请显式指定 `--serial`。

- 点击图像窗口后按 **S**：保存未画框BGR PNG及同名灰度统计CSV。
- 按 **Q** 或 **Esc**，或正常关闭图像窗口：退出并清理；等待 `SDK released` 后结束操作。
- 原图保持未画框，检测框、类别、置信度显示在结果窗口。带框证据另用系统截图/录屏保存。
- `captures/`、`logs/`、`models/` 基于编译时源文件所在目录定位。在新的克隆目录重新编译即可；仅移动旧可执行文件不会改变这些路径。
- CLion强制停止可能使缓冲日志来不及落盘。早期两份空日志保留作历史记录，不作为有效参数/清理证据。

曝光单位为μs，增益单位为dB。指定参数后关闭对应自动调节，检查设备范围并打印读回值；不指定的参数保留设备当前设置。实验中的6/12 dB实际读回为5.9953/11.9906 dB，以记录为准。

## 各实验命令

曝光测试，保持增益0，分别将曝光换成500、1000、3000：

```bash
./build/work --serial DA4302122 --exposure 500 --gain 0
```

增益测试，曝光固定1000，增益分别0、6、12：

```bash
./build/work --serial DA4302122 --exposure 1000 --gain 6
```

原生格式性能测试，实际相机均支持；关闭YOLO与显示：

```bash
./build/work --serial DA4302122 --exposure 1000 --gain 0 --format BayerRG8 --frames 300 --headless --no-yolo
./build/work --serial DA4302122 --exposure 1000 --gain 0 --format BGR8 --frames 300 --headless --no-yolo
```

内存示例与受控异常测试：

```bash
./build/mat_memory
./build/work --serial DA4302122 --frames 30 --headless --no-yolo --simulate timeout
./build/work --serial DA4302122 --frames 30 --headless --no-yolo --simulate format
```

两项错误在正常处理3帧后注入，预期进程退出码1。timeout借出/归还3/3，format借出/归还4/4，并均完成清理。测试后恢复正常采集实参，删除 `--simulate`。

本地视频测试，替换成自己的视频路径：

```bash
./build/work --video /absolute/path/to/video.mp4
./build/work --video /absolute/path/to/video.mp4 --headless
```

程序不自动录制演示视频；仓库中的录像来自Ubuntu录屏。`--headless`只关闭本程序窗口，不保证SDK虚拟设备可脱离桌面运行。

## 图像、模型与内存流程

```text
初始化SDK → 枚举USB设备 → 创建/打开句柄 → 设置参数 → 开始采集
    → 借用SDK帧 → 转换/复制到独立BGR内存 → 归还SDK帧
    → letterbox预处理 → YOLO推理 → 按类别NMS → 画框/显示/保存
退出：停止采集 → 关闭设备 → 销毁句柄 → 释放SDK
```

支持紧密排列的RGB8、BGR8、BayerRG8、Mono8。RGB交换通道，Bayer使用 `COLOR_BayerRGGB2BGR`，Mono转BGR；原生BGR通过clone获得独立内存。尚未实现带行填充、Chunk、Multipart等特殊布局。

模型输入1×3×640×640，输出1×18×8400。预处理按比例缩放、填充值114、BGR转RGB并除以255；置信度阈值0.35，按类别NMS IoU 0.70，最多300框。类别为B1、B2、B3、R1、R2、R3。模型含关键点输出，当前只绘制框、类别和置信度。

## 统计口径

- 保存图灰度饱和比例：BGR转灰度后，灰度≥250的像素数/总像素数×100%。这是统一近似指标，不等同于任意颜色通道饱和率。
- FPS：采集处理循环吞吐率，包含取帧等待及启用的处理/显示/保存开销，不是相机标称帧率。
- CPU：整个进程各线程CPU时间/墙钟时间，单逻辑核满载100%，可以超过100%。
- 格式转换与BGR输入的独立内存复制分别计时。
- `mean_yolo_pipeline_ms`包含预处理、推理和画框；`mean_get_to_result_ms`为平均取帧开始至处理结果产生的时间，不是纯模型延迟。
- `status`为采集阶段状态，后续关闭/销毁/释放还需查看日志。
- 实验中仅有单次格式采样，目标构图与部分环境信息未固定记录，结论按实测范围表述。

## 上传GitHub

此目录已包含源码、模型和证据，无需上传 `build/`、CLion配置或SDK安装目录。检查 [待补清单](docs/submission_checklist.md) 后，可在此目录执行：

```bash
git init
git add .
git commit -m "Add industrial camera project and experiment evidence"
git branch -M main
```

然后在自己的GitHub账号建立仓库，按仓库页面提示设置远程地址并push。不要把占位仓库地址当成真实地址执行。当前交付仅完成本地整理，尚未创建远程仓库或上传。
