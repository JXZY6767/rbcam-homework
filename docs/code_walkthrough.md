# 程序主要步骤说明

文件行号对应本次归档的main.cpp，修改源码后可能变化。

| 位置 | 做什么 | 为什么需要 |
| --- | --- | --- |
| main.cpp:25 / :58，TeeBuffer / RunLog | 程序输出同时写终端和日志 | 留下参数、错误与清理证据；正常退出才保证缓冲日志落盘 |
| main.cpp:95 / :138，CameraOptions / parseCameraOptions | 解析程序实参，检查数值 | 把曝光、增益、序列号等交给SDK |
| main.cpp:203 / :234，setAndReadFloat / configureCamera | 关闭自动调节，按范围设置并读回 | 记录实际采用值，不能只相信输入数字 |
| main.cpp:304 / :353，CaptureStatistics / savePerformanceSummary | 时间、帧数、CPU与缓冲区统计 | 区分设备等待、转换、复制和识别开销 |
| main.cpp:393，convertFrameToBgr | 检查格式/长度并转换或深拷贝 | SDK缓冲区归还后仍有可用像素 |
| main.cpp:487 / :510，calculateImageStatistics / saveFrame | 保存未画框PNG，计算灰度与饱和比例 | 对比参数时避免检测框影响图像统计 |
| main.cpp:564，drawDetections | 解析6类分数、按类别NMS、还原坐标、画框 | 正确把640输入坐标映射回原图 |
| main.cpp:654 / :671，loadYoloModel / inferAndDraw | 加载一次模型，逐帧预处理与推理 | 连续识别，而非只处理一张本地图 |
| main.cpp:734，runVideo | 读取离线视频，复用模型流程 | 不连接SDK也能验证推理链路 |
| main.cpp:819，captureLoop | 借帧、转换、归还、识别、显示与退出 | 每帧处理及异常路径的核心 |
| main.cpp:999，runCamera | 创建/打开相机，结束时关闭/销毁 | 资源生命周期管理 |
| main.cpp:1048 / :1149，runApplication / main | 初始化/枚举/选择设备，最终释放SDK | 程序入口与顶层错误报告 |

## C语言基础如何理解Mat

`cv::Mat`可以理解为图像说明信息加上像素内存指针。`alias = view`主要复制说明信息，指针仍指向同一块像素；`clone()`另外分配像素并复制数据。包住SDK指针的Mat不拥有SDK内存，不能在归还后继续读。

当前代码在归还SDK帧前转为独立BGR图像，随后推理只访问独立图像。格式错误路径也要归还已经借到的那一帧。超时注入发生在下一次借帧之前，所以没有第4次借用；格式错误发生在借用第4帧之后，所以归还数为4。

## 识别输出

模型输出1×18×8400：前4项是框坐标，接着6项是类别分数，其余是关键点。程序按当前模型格式解析，没有额外把一个不存在的objectness项乘入分数。NMS按类别独立处理，最后把框映射回原图。
