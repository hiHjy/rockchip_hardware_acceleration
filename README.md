# Rockchip Hardware Acceleration Core

这个仓库只保留 Rockchip 硬件加速的核心封装，不包含项目里的 StreamManager、WebRTC、VideoView 和业务接入层。

## 目录

- `mpp/mpp_simple.*`
  - 通用流式 MPP 解码：H.264/H.265/其他连续码流输入，输出 MPP frame / dma-buf fd。
  - MPP 编码：NV12/RGB 等 dma-buf fd 输入，输出 H.264/H.265 packet。
- `mpp/mpp_advance.*`
  - MJPEG dma-buf 单帧解码成 NV12 dma-buf。
  - 适合 V4L2 摄像头 MJPEG 输入。
- `rga/rk_rga.*`
  - RGA 裁剪、缩放、旋转、镜像、颜色空间转换、合成。
- `rga/include/`
  - RGA im2d 头文件。
- `common/image.h`
  - RGA 参数需要的最小图像枚举和结构体。

## 不包含

- MPP/RGA 的项目适配层
- `hw_accel.*`
- `stream_manager.*`
- WebRTC / UI / DRM 显示业务代码

这些都属于项目接入层，不放进核心硬件加速仓库。

## 编译依赖

目标板或 sysroot 需要有：

- Rockchip MPP 头文件和库
- librga 头文件和库
- libdrm 头文件和库
- pthread

常见链接库：

```bash
-lrockchip_mpp -lrga -ldrm -lpthread
```

## 语法检查示例

```bash
aarch64-linux-gnu-gcc -fsyntax-only -std=gnu11 \
  -I/home/hjy/rk3568_sysroot_fixed/usr/include \
  -I/home/hjy/rk3568_sysroot_fixed/usr/include/rockchip \
  -I/home/hjy/rk3568_sysroot_fixed/usr/include/libdrm \
  mpp/mpp_simple.c mpp/mpp_advance.c

aarch64-linux-gnu-g++ -fsyntax-only -std=c++17 \
  -I/home/hjy/rk3568_sysroot_fixed/usr/include \
  -I/home/hjy/rk3568_sysroot_fixed/usr/include/libdrm \
  -Irga \
  rga/rk_rga.cpp
```
