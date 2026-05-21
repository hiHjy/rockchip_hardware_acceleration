/***********************************************************************2d图像硬件加速定义 ************/
#ifndef __RK_RGA_H__
#define __RK_RGA_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "../common/image.h"


// 2d图像硬件加速上下文
typedef void* RkRgaCtx;

/**
 * 初始化2d图像硬件加速，1.内部启动用一个独立的线程用于执行2d图像硬件加速任务;2.初始化硬件
 * @param out ctx 2d图像硬件加速上下文
 * @return 0 成功，其他失败
 */
int rk_rga_init(RkRgaCtx *ctx);

/**
 * 释放2d图像硬件加速资源
 * @param in ctx 2d图像硬件加速上下文
 * @return 0 成功，其他失败
 */
int rk_rga_release(RkRgaCtx ctx);

// 2d图像硬件加速操作码
enum RgaOpEnum {
    // 无操作
    RgaOpNone = 0,
    // 旋转
    RgaOpRotate = 1 << 0,
    // 缩放
    RgaOpScale = 1 << 1,
    // 裁剪
    RgaOpCrop = 1 << 2,
    // 镜像
    RgaOpMirror = 1 << 3,
    // 颜色空间转换
    RgaOpConvertColorSpace = 1 << 4,
    // 分辨率转换
    // RgaOpConvertResolution = 1 << 5,
    // 清除内存对齐部分的数据 
    RgaOpCleanAlignment = 1 << 6,
    // 图片合并，把src图片合并到dest图片中
    RgaOpMerge = 1 << 7,
};

/**
 * 计算指定format的图像字节大小
 * @param in format 图像格式
 * @param in width 图像宽度
 * @param in height 图像高度
 * @return 图像字节大小
 */
int rk_rga_compute_bytes_of_format(int format, int width, int height);


/**
 * 2d图像硬件加速结果
 */
typedef struct {
    // 硬件加速返回的 dma-fence fd，用于等待硬件加速完成
    // 等待完成后，需要关闭 fenceFd 释放资源
    // 若返回 -1 表示操作失败或无 fence 支持
    int fenceFd;
    // 输出图像的宽度
    int width;
    // 输出图像的高度
    int height;
    // 输出图像的行方向 stride（按像素计）
    int strideWidth;
    // 输出图像的列方向 stride（按像素计）
    int strideHeight;
    // 输出图像的字节大小
    int size;
} RgaResult;

/**
 * 2d图像硬件加速参数
 */
typedef struct {
    // 父级 dma-fence fd，用于等待父级任务完成
    // 该 fd 将被传递给 RGA 驱动作为 acquire_fence_fd
    // RGA 驱动会在开始处理前等待此 fence signal
    int parentFenceFd;
    // dma-bufFd，用于输入图像的dma-bufFd
    int srcDmaBufFd;
    // 输入图像的dma-bufFd大小
    int srcDmaBufSize;
    // 输入图像的格式
    ColorSpaceEnum srcFormat;
    // 输入图像的实际宽度
    int srcWidth;
    // 输入图像的实际高度
    int srcHeight;
    // 输入图像的对齐长度（按像素计）
    int srcStrideWidth;
    // 输入图像的高度对齐长度（按像素计）
    int srcStrideHeight;
    // 输出dma-bufFd
    int dstDmaBufFd;
    // 输出dma-bufFd空间容量 必须使用操作过程中最大值进行内存分配，否则会导致内存泄漏
    int dstDmaBufCapacity;
    // 输出图像的内存对齐步长单元，比如2对齐、4对齐等，默认值为2对齐. 0表示不处理对齐操作
    int dstWidthAlignmentUnit;  
    // 输出图像的高度内存对齐步长单元，比如2对齐、4对齐等，默认值为2对齐. 0表示不处理对齐操作
    int dstHeightAlignmentUnit;  
    // 输出图像的格式
    ColorSpaceEnum dstFormat;
    // 旋转角度, 只支持0, 90, 180, 270度, 0为不旋转
    ImageRotationEnum rotateAngle;
    // 缩放比例, 1表示不缩放，需要内部自己计算真实缩放分辨率。计算完缩放的分辨率后，需要把缩放的分辨率写入result结果中
    float scaleRate;
    // 裁剪区域,具体的裁剪大小需要根据硬件的对齐规则来，裁剪确定需要把裁剪后的分辨率写入result结果中
    ImageRect cropRect;
    // 目标分辨率
    // ImageResolution resolution;
    // 镜像操作
    ImageMirrorEnum mirror;
    // 图片合并坐标点, 把src图片合并到dest图片中，合并到dest图片的坐标点
    ImageMerge merge;
    // 操作码数量
    int opCount;
    // 操作码集合，最多支持9个操作码。操作码在数组中的顺序表示操作码的执行顺序，但是具体的实现顺序需要根据硬件的实现来确定
    RgaOpEnum ops[9];
} RgaParam;

/**
 * 提交2d图像硬件加速任务
 * @param in ctx 2d图像硬件加速上下文
 * @param in param 输入图像参数
 * @param out result 输出图像结果
 * @return 0 成功，其他失败
 */
int rk_rga_submit_task(RkRgaCtx ctx, const RgaParam *param, RgaResult *result);


#ifdef __cplusplus
}
#endif

#endif // __RK_RGA_H__
