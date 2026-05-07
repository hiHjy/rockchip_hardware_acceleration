#ifndef RKMPP_MJPEG_H
#define RKMPP_MJPEG_H

#include <stddef.h>

#include <rk_mpi.h>
#include <mpp_buffer.h>
#include <rk_vdec_cfg.h>

/*
 * 内部输出 buffer 槽位数量。
 *
 * 只有在 rk_mpp_mjpeg_decoder_decode_dmafd() 的 dstfd < 0 时才会用到。
 * 常见摄像头/V4L2 链路本来就有固定数量的 buffer index，所以这里按 index
 * 缓存一组可复用的 NV12 输出 dma-buf，避免每帧反复申请和释放。
 */
#define RKMPP_MJPEG_OUTPUT_SLOT_COUNT 4

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 单帧 MJPEG 解码完成后的回调。
 *
 * fd:
 *   解码后的 NV12 dma-buf fd。
 *   - 如果调用 decode_dmafd() 时传入了外部 dstfd，这里返回的就是那个 fd。
 *   - 如果传入 dstfd < 0，这里返回的是模块内部申请的 fd。
 *
 * index:
 *   调用方传入的槽位号。内部输出模式下，它决定复用哪一个内部输出 fd。
 *
 * size:
 *   MPP 返回的输出 buffer 大小。读取 NV12 时不要只看 width/height，
 *   要按 hor_stride/ver_stride 计算 Y 和 UV 平面地址。
 */
typedef void (*RkMppMjpegFrameCallback)(int fd,
                                        int index,
                                        RK_U32 width,
                                        RK_U32 height,
                                        RK_U32 hor_stride,
                                        RK_U32 ver_stride,
                                        size_t size,
                                        void *userdata);

typedef struct RkMppMjpegDecoder {
    /* MPP 解码器上下文和 API 表。 */
    MppCtx dec_ctx;
    MppApi *dec_api;

    /*
     * 保留字段，用来兼容之前的实验版本。
     * 当前每帧 import 出来的 MppBuffer 会在 decode_dmafd() 内部释放。
     */
    MppBuffer in_buf;
    MppBuffer out_buf;
    MppBufferInfo in_buf_info;
    MppBufferInfo out_buf_info;

    int dec_initialized;
    int buf_is_init;

    /*
     * 内部持有的输出 dma-buf。
     *
     * 只有 dstfd < 0 时才会使用这里的 fd。
     * 外部传进来的 dstfd 不会存到这里，也不会由 deinit 关闭。
     */
    int dst_fd[RKMPP_MJPEG_OUTPUT_SLOT_COUNT];
    size_t dst_size[RKMPP_MJPEG_OUTPUT_SLOT_COUNT];

    RkMppMjpegFrameCallback frame_callback;
    void *frame_callback_userdata;
} RkMppMjpegDecoder;

void rk_mpp_mjpeg_decoder_set_frame_callback(RkMppMjpegDecoder *ctx,
                                             RkMppMjpegFrameCallback callback,
                                             void *userdata);

/*
 * 初始化 MJPEG task 模式解码器。
 *
 * 正常使用传 MPP_VIDEO_CodingMJPEG。coding 参数保留下来是为了方便做 MPP
 * 行为对比测试，但这个封装本身是按“一包 MJPEG 输入 -> 一帧 NV12 输出”
 * 设计的。
 */
int rk_mpp_mjpeg_decoder_init(RkMppMjpegDecoder *ctx, MppCodingType coding);

/*
 * 释放解码器和内部申请的输出 dma-buf。
 *
 * 调用方自己传入的外部 dstfd 不会在这里关闭，fd 所有权仍然属于调用方。
 */
void rk_mpp_mjpeg_decoder_deinit(RkMppMjpegDecoder *ctx);

/*
 * 从输入 dma-buf 解码一包完整 MJPEG/JPEG 到一帧 NV12 输出 dma-buf。
 *
 * fd:
 *   输入 dma-buf，里面必须是一帧完整的 JPEG/MJPEG 压缩数据。
 *
 * dstfd:
 *   >= 0：解码到调用方提供的输出 dma-buf。调用方负责保证容量足够，
 *         也负责关闭这个 fd。
 *   <  0：模块内部按 index 申请/复用输出 dma-buf。解码完成后的 fd
 *         通过 frame callback 返回。
 *
 * index:
 *   槽位号，用于日志和回调。内部输出模式下必须在
 *   [0, RKMPP_MJPEG_OUTPUT_SLOT_COUNT) 范围内。
 *
 * w/h:
 *   JPEG 头解析失败时使用的兜底可见宽高。解析成功时，以 JPEG 头里的宽高为准。
 *
 * stride:
 *   为了兼容摄像头回调保留。MJPEG 输出 stride 会根据解析/兜底宽高重新计算。
 *
 * size:
 *   当前 MJPEG 压缩数据的真实字节数，不是整个 dma-buf 的容量。
 */
int rk_mpp_mjpeg_decoder_decode_dmafd(RkMppMjpegDecoder *ctx,
                                      int fd,
                                      int dstfd,
                                      int index,
                                      int w,
                                      int h,
                                      int stride,
                                      int size);

#ifdef __cplusplus
}
#endif
#endif
