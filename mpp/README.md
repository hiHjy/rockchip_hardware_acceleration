# MPP 硬件加速封装说明

这个目录放的是 Rockchip MPP 的轻量 C 封装，按数据流分成三类：

- `rkmpp_enc.*`：把 NV12 等原始图像 dma-buf 编码成 H.264/H.265 码流。
- `rkmpp_dec.*`：通用流式解码器，适合 H.264 Annex-B、H.265 等连续码流。
- `rkmpp_mjpeg.*`：MJPEG 专用路径，一帧 MJPEG dma-buf 输入，一帧 NV12 dma-buf 输出。

这里不包含 `rga/` 的使用说明。

## 编译检查

这些文件依赖目标板 sysroot 里的 Rockchip MPP 和 libdrm 头文件。单独做语法检查可以这样：

```bash
cc -fsyntax-only -std=gnu11 \
  -I/home/hjy/rk3568_sysroot_fixed/usr/include \
  -I/home/hjy/rk3568_sysroot_fixed/usr/include/rockchip \
  -I/home/hjy/rk3568_sysroot_fixed/usr/include/libdrm \
  mpp/rkmpp_enc.c mpp/rkmpp_dec.c mpp/rkmpp_mjpeg.c
```

实际链接应用时，还需要按你的 sysroot 链接 MPP 和 DRM 库，常见是：

```bash
-lrockchip_mpp -ldrm
```

具体库名和路径以目标板 rootfs 为准。

## H.264 / H.265 流式解码

H.264 Annex-B、H.265 这类真实视频流应该使用 `rkmpp_dec.*`。

这条路径使用 MPP 的：

```text
decode_put_packet()
decode_get_frame()
```

并且在非 MJPEG codec 下开启 `base:split_parse`。这更适合 Annex-B 这种连续字节流，因为一帧可能跨 packet，或者一个 packet 里也可能包含多段码流。

基本用法：

```c
#include "mpp/rkmpp_dec.h"

static void on_frame(const uint8_t *data,
                     size_t size,
                     int fd,
                     RK_U32 width,
                     RK_U32 height,
                     RK_U32 h_stride,
                     RK_U32 v_stride,
                     RK_U32 fmt,
                     RK_S64 pts_us,
                     void *userdata)
{
    /*
     * fd 是 MPP 输出帧对应的 dma-buf fd。
     * data 是 MPP 能映射出 CPU 地址时返回的指针。
     * 读取 NV12 时要按 h_stride/v_stride 算平面地址，不能只按 width/height。
     */
}

RkMppDecoder dec;

rk_mpp_decoder_init(&dec, MPP_VIDEO_CodingAVC, NULL);
rk_mpp_decoder_set_frame_callback(&dec, on_frame, NULL);

/* packet 是 H.264 Annex-B 数据，packet_size 是真实有效字节数。 */
rk_mpp_decoder_send_data_with_pts(&dec, packet, packet_size, 0, pts_us);

/* 码流结束时送 eos。 */
rk_mpp_decoder_send_data(&dec, NULL, 0, 1);
rk_mpp_decoder_deinit(&dec);
```

注意：

- `MPP_VIDEO_CodingAVC` 表示 H.264。
- `MPP_VIDEO_CodingHEVC` 表示 H.265。
- 当前封装会把输入 packet 复制到内部缓冲区，大小由 `RKMPP_DEC_INPUT_BUF_SIZE` 控制。
- 输出 buffer group 由封装内部准备。

## NV12 Dma-Buf 编码

已有原始图像 dma-buf 时，使用 `rkmpp_enc.*` 编码。例如摄像头 NV12 帧、RGA 输出帧等。

基本用法：

```c
#include "mpp/rkmpp_enc.h"

static void on_packet(const uint8_t *data,
                      size_t size,
                      int is_header,
                      int eos,
                      void *userdata)
{
    /*
     * is_header 为 1 时表示编码头，例如 H.264 的 SPS/PPS。
     * 普通编码帧 is_header 为 0。
     */
}

RkMppEncoder enc;

rk_mpp_encoder_init(&enc,
                    MPP_VIDEO_CodingAVC,
                    width,
                    height,
                    0,       /* 自动按 16 对齐 hor_stride */
                    0,       /* 自动按 16 对齐 ver_stride */
                    MPP_FMT_YUV420SP,
                    30,      /* fps */
                    0,       /* 自动码率 */
                    0,       /* 自动 gop */
                    NULL);

rk_mpp_encoder_set_packet_callback(&enc, on_packet, NULL);
rk_mpp_encoder_write_header(&enc);

/* input_fd 必须是一帧和初始化参数匹配的 NV12 图像。 */
rk_mpp_encoder_send_frame(&enc, input_fd, 0);

/* 最后一帧可以带 eos。 */
rk_mpp_encoder_send_frame(&enc, input_fd, 1);
rk_mpp_encoder_deinit(&enc);
```

## MJPEG Dma-Buf 单帧解码

`rkmpp_mjpeg.*` 专门处理摄像头常见场景：

```text
一帧 MJPEG/JPEG 压缩数据所在的 dma-buf
    -> MPP 解码
一帧 NV12 输出 dma-buf
```

这条路径使用 MPP task 模式，核心关系是：

```text
输入 MJPEG fd -> MppBuffer -> MppPacket
输出 NV12 fd  -> MppBuffer -> MppFrame
task 提交/取回 -> 回调返回解码后的 NV12 fd
```

它不是 H.264 Annex-B 的主解码路径。H.264/H.265 请用 `rkmpp_dec.*`。

### 外部输出 fd 模式

如果你已经有输出 dma-buf，就把它作为 `dstfd` 传进去。

这种模式下：

- MPP 会把 NV12 解码结果写到你传入的 `dstfd`。
- 模块不会保存这个 fd。
- 模块不会在 `deinit` 时关闭这个 fd。
- 调用方必须保证这个 fd 的容量足够大。

示例：

```c
#include "mpp/rkmpp_mjpeg.h"

static void on_mjpeg_frame(int fd,
                           int index,
                           RK_U32 width,
                           RK_U32 height,
                           RK_U32 h_stride,
                           RK_U32 v_stride,
                           size_t size,
                           void *userdata)
{
    /*
     * 外部输出模式下，fd 就是调用 decode_dmafd() 时传入的 dstfd。
     * 后续 RGA / 显示 / 保存文件都应该按 stride 读取这块 NV12。
     */
}

RkMppMjpegDecoder dec;

rk_mpp_mjpeg_decoder_init(&dec, MPP_VIDEO_CodingMJPEG);
rk_mpp_mjpeg_decoder_set_frame_callback(&dec, on_mjpeg_frame, NULL);

rk_mpp_mjpeg_decoder_decode_dmafd(&dec,
                                  mjpeg_fd,
                                  output_fd,   /* 外部提供的输出 fd */
                                  v4l2_index,
                                  width,
                                  height,
                                  stride,
                                  mjpeg_size);

rk_mpp_mjpeg_decoder_deinit(&dec);
```

参数说明：

- `mjpeg_fd`：输入 dma-buf，里面必须是一帧完整 JPEG/MJPEG。
- `output_fd`：输出 dma-buf，调用方自己申请和关闭。
- `v4l2_index`：槽位号，用于日志和回调。
- `width/height`：JPEG 头解析失败时的兜底宽高；解析成功时以 JPEG 头为准。
- `stride`：为了兼容摄像头回调保留，MJPEG 输出 stride 会内部重新计算。
- `mjpeg_size`：当前压缩帧的真实字节数，不是整个 buffer 容量。

### 内部输出 fd 模式

如果你没有输出 dma-buf，就把 `dstfd` 传 `-1`。

模块会按 `index` 申请或复用内部输出 fd：

```c
rk_mpp_mjpeg_decoder_decode_dmafd(&dec,
                                  mjpeg_fd,
                                  -1,          /* 使用内部输出 fd */
                                  v4l2_index,
                                  width,
                                  height,
                                  stride,
                                  mjpeg_size);
```

内部输出模式规则：

- `index` 必须在 `[0, RKMPP_MJPEG_OUTPUT_SLOT_COUNT)` 范围内。
- 每个 `index` 最多缓存一个内部输出 fd。
- 如果缓存的 fd 容量够，就复用。
- 如果新帧需要更大的输出空间，就关闭旧 fd，重新申请更大的 fd。
- 内部 fd 会在 `rk_mpp_mjpeg_decoder_deinit()` 里关闭。
- 回调里拿到的 fd 属于 decoder，不能由调用方关闭。
- 回调里拿到的 fd 在同一个 `index` 被重新分配前有效。

内部申请输出 fd 的策略：

1. 优先从 DRM dumb buffer 导出 dma-buf。默认设备是 `/dev/dri/card0`，也可以通过 `DRM_DEVICE` 环境变量指定。
2. DRM 失败时，依次尝试常见 dma_heap 节点，例如 `/dev/dma_heap/system`、`/dev/dma_heap/cma` 等。

## 怎么选

- H.264 Annex-B / H.265 / 连续视频流：用 `rkmpp_dec.*`。
- 摄像头一帧 MJPEG dma-buf 解成一帧 NV12：用 `rkmpp_mjpeg.*`。
- NV12 原始图像编码成 H.264/H.265：用 `rkmpp_enc.*`。

简单说：`rkmpp_dec.*` 管“流”，`rkmpp_mjpeg.*` 管“一帧 MJPEG 到一帧 NV12”，`rkmpp_enc.*` 管“原始帧到压缩码流”。
