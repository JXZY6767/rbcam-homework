# 模型说明

- 文件：`best.onnx`，本仓库已包含，大小 10147151 字节。
- 来源：用户已有作业包 `10.18work/models/best.pt`；未训练新模型，未获知原训练集与训练配置，不补写训练指标或授权信息。
- 导出为ONNX，opset12，FP32，固定输入1×3×640×640，输出1×18×8400；不内嵌NMS。
- 类别：0 B1、1 B2、2 B3、3 R1、4 R2、5 R3。输出还包含姿态关键点，当前C++程序只显示框/类别/置信度。
- 推理：OpenCV DNN CPU。SHA-256：`bcfba0ae791ba8c93ad874368a5b0dfee6c6eef42ecf9ec4b974271554a99f87`。

成功导出环境：Python3.14.4、torch2.9.1+debian、torchvision0.24.1+cpu、ultralytics8.4.183、onnx1.20.0。运行C++不需要这些Python包。

此前导出方式，模型路径按本地文件替换：

```python
from ultralytics import YOLO
model = YOLO('/absolute/path/to/best.pt', task='pose')
model.export(format='onnx', imgsz=640, batch=1, device='cpu',
             opset=12, simplify=False, dynamic=False, nms=None)
```

原始权重的使用范围应遵循提供方要求；仓库未对第三方模型和SDK另行声明开源许可证。
