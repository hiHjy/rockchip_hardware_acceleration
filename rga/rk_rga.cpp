/**
 * rk_rga.cpp
 * rk_rga.h 的实现：RGA 2D 硬件加速核心。
 *
 * 架构：
 *   - 一个RGA 任务执行从 FIFO 队列处理任务。
 *   - 每个提交的任务都会收到一个 dma-fence fd 作为 fenceFd（通过 RgaResult 返回）。
 *   - 调用者可以轮询/读取 fenceFd 来等待硬件完成，然后关闭它。
 *   - parentFenceFd（上游 dma-fence）通过 acquire_fence_fd 传递给 RGA 驱动。
 *   - 所有操作（裁剪、缩放、旋转、镜像）组合成一个 improcess() 异步调用。
 */

#include "rk_rga.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <pthread.h>
#include <poll.h>
#include <unistd.h>
#include <sys/mman.h>

/* RGA im2d API (installed under rga/include/) */
#include "include/im2d.h"
#include "include/im2d_mpi.h"
#include "include/rga.h"
#include "include/RgaApi.h"

// ============================================================
// 格式帮助函数
// ============================================================

static int dma_fmt_to_rga(int dma_fmt)
{
    switch (static_cast<ColorSpaceEnum>(dma_fmt))
    {
    case ColorSpace_NV12:
        return RK_FORMAT_YCbCr_420_SP;
    case ColorSpace_RGB:
        return RK_FORMAT_RGB_888;
    case ColorSpace_BGR:
        return RK_FORMAT_BGR_888;
    case ColorSpace_YUV420P:
        return RK_FORMAT_YCbCr_420_P;
    case ColorSpace_GRAY:
        return RK_FORMAT_YCbCr_400;
    case ColorSpace_RGBA:
        return RK_FORMAT_RGBA_8888;
    default:
        fprintf(stderr, "[RkRga] error: Unknown dma_fmt: %d\n", dma_fmt);
        return -1;
    }
}

static int rotation_to_usage(int angle)
{
    switch (angle)
    {
    case 90:
        return IM_HAL_TRANSFORM_ROT_90;
    case 180:
        return IM_HAL_TRANSFORM_ROT_180;
    case 270:
        return IM_HAL_TRANSFORM_ROT_270;
    default:
        return 0;
    }
}

static int align_up(int value, int alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static int dimension_alignment(int format)
{
    switch (static_cast<ColorSpaceEnum>(format))
    {
    case ColorSpace_RGB:
    case ColorSpace_BGR:
        return 4;
    case ColorSpace_NV12:
    case ColorSpace_YUV420P:
        return 2;
    default:
        return 1;
    }
}

static const char *color_space_to_string(int format)
{
    switch (static_cast<ColorSpaceEnum>(format))
    {
    case ColorSpace_NONE:
        return "NONE";
    case ColorSpace_RGB:
        return "RGB888";
    case ColorSpace_BGR:
        return "BGR888";
    case ColorSpace_NV12:
        return "NV12";
    case ColorSpace_GRAY:
        return "GRAY";
    case ColorSpace_YUV420P:
        return "YUV420P";
    case ColorSpace_MJPEG:
        return "MJPEG";
    case ColorSpace_YUYV:
        return "YUYV";
    case ColorSpace_RGBA:
        return "RGBA8888";
    default:
        return "UNKNOWN";
    }
}

static const char *rga_op_to_string(RgaOpEnum op)
{
    switch (op)
    {
    case RgaOpRotate:
        return "Rotate";
    case RgaOpScale:
        return "Scale";
    case RgaOpCrop:
        return "Crop";
    case RgaOpMirror:
        return "Mirror";
    case RgaOpConvertColorSpace:
        return "ConvertColorSpace";
    // case RgaOpConvertResolution:
    //     return "ConvertResolution";
    case RgaOpMerge:
        return "Merge";
    default:
        return "Unknown";
    }
}

static int rga_op_priority(RgaOpEnum op)
{
    switch (op)
    {
    case RgaOpCrop:
    // case RgaOpConvertResolution:
    //     return 0;
    case RgaOpScale:
        return 1;
    case RgaOpRotate:
        return 2;
    case RgaOpMirror:
        return 3;
    case RgaOpConvertColorSpace:
        return 4;
    default:
        return 100;
    }
}

static bool normalize_rga_ops(const RgaParam *param, RgaOpEnum normalized_ops[6], int *normalized_count)
{
    if (normalized_count == nullptr)
        return false;

    *normalized_count = 0;
    if (param == nullptr)
        return false;

    // 这一步做的是“操作序列归一化”。
    // 上层可能传来任意顺序，但当前实现希望把状态推导固定成一套稳定规则：
    // 1. Crop / ConvertResolution
    // 2. Scale
    // 3. Rotate
    // 4. Mirror
    // 5. ConvertColorSpace
    //
    // 这样 do_rga_task() 里后面的状态机就不会因为上层乱序而产生歧义。
    bool has_crop = false;
    bool has_convert_resolution = false;

    for (int i = 0; i < param->opCount && i < 6; ++i)
    {
        if (param->ops[i] == RgaOpCrop)
            has_crop = true;
        // if (param->ops[i] == RgaOpConvertResolution)
        //     has_convert_resolution = true;
    }

    if (has_crop && has_convert_resolution)
    {
        // 这两个操作在当前实现里都会重写 src_crop_* 和 out_w/out_h，
        // 语义上天然冲突，所以直接报错而不是让后者默默覆盖前者。
        fprintf(stderr, "[RkRga] Crop and ConvertResolution cannot coexist in one task，就是无法共存！\n");
        return false;
    }

    bool used[6] = {false, false, false, false, false, false};
    for (int priority = 0; priority <= 4; ++priority)
    {
        for (int i = 0; i < param->opCount && i < 6; ++i)
        {
            if (used[i])
                continue;
            if (rga_op_priority(param->ops[i]) != priority)
                continue;

            normalized_ops[(*normalized_count)++] = param->ops[i];
            used[i] = true;
        }
    }

    for (int i = 0; i < param->opCount && i < 6; ++i)
    {
        if (used[i])
            continue;
        normalized_ops[(*normalized_count)++] = param->ops[i];
        used[i] = true;
    }

    return true;
}

static void print_rga_ops_if_reordered(const RgaParam *param, const RgaOpEnum normalized_ops[6], int normalized_count)
{
    if (param == nullptr)
        return;

    bool changed = normalized_count != param->opCount;
    if (!changed)
    {
        for (int i = 0; i < normalized_count && i < 6; ++i)
        {
            if (normalized_ops[i] != param->ops[i])
            {
                changed = true;
                break;
            }
        }
    }

    if (!changed)
        return;

    fprintf(stdout, "[RkRga] 操作序列重新排序:");
    for (int i = 0; i < param->opCount && i < 6; ++i)
    {
        fprintf(stdout, " %s", rga_op_to_string(param->ops[i]));
    }
    fprintf(stdout, " ->");
    for (int i = 0; i < normalized_count && i < 6; ++i)
    {
        fprintf(stdout, " %s", rga_op_to_string(normalized_ops[i]));
    }
    fprintf(stdout, "\n");
}

int rk_rga_compute_bytes_of_format(int format, int width, int height)
{
    switch (static_cast<ColorSpaceEnum>(format))
    {
    case ColorSpace_NV12:
    case ColorSpace_YUV420P:
        return width * height * 3 / 2;
    case ColorSpace_RGB:
    case ColorSpace_BGR:
        return width * height * 3;
    case ColorSpace_RGBA:
        return width * height * 4;
    case ColorSpace_GRAY:
        return width * height * 1;
    default:
        return -1;
    }
}

// ============================================================
// 内部任务/上下文结构
// ============================================================

struct RgaTask
{
    RgaParam param;
    RgaResult *result;
    RgaTask *next;
};

struct RkRgaInternal
{
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;      /* 任务队列非空通知 */
    pthread_cond_t done_cond; /* 任务完成通知 */
    RgaTask *head,
        *tail;
    int running;
};

// ============================================================
// 任务执行
// ============================================================

// dma-fence 由 RGA 驱动在硬件完成后自动 signal，无需手动触发

static void do_rga_task(RgaTask *task)
{
    const RgaParam *param = &task->param;
    RgaResult *result = task->result;

    int src_rga_fmt = dma_fmt_to_rga(param->srcFormat);
    int dst_rga_fmt = dma_fmt_to_rga(param->dstFormat);

    if (src_rga_fmt < 0 || dst_rga_fmt < 0)
    {
        fprintf(stderr, "[RkRga] Unsupported fmt src=%d dst=%d\n",
                param->srcFormat, param->dstFormat);
        result->fenceFd = -1;
        return;
    }

    // 下面这几组变量是整个函数的核心状态：
    //
    // 1. src_crop_*：最终从源图取哪一块
    // 2. out_w/out_h：最终可见输出尺寸
    // 3. usage：传给 RGA 的变换标志，比如旋转、镜像
    //
    // 可以把它理解成一个“小型状态机”：
    // 初始状态 = 整张源图原样输出
    // 每处理一个 op，就在当前状态上继续推导
    // 循环结束后留下来的状态，就是最终提交给 improcess() 的参数语义
    int usage = IM_SYNC;
    int src_crop_x = 0;
    int src_crop_y = 0;
    int src_crop_w = param->srcWidth;
    int src_crop_h = param->srcHeight;
    int out_w = param->srcWidth;
    int out_h = param->srcHeight;
    // 前景图片的合并坐标点
    int fg_bgX = 0;
    int fg_bgY = 0;
    bool fg_existed = false;


    RgaOpEnum normalized_ops[9] = {static_cast<RgaOpEnum>(0)};
    int normalized_count = 0;

    if (!normalize_rga_ops(param, normalized_ops, &normalized_count))
    {
        result->fenceFd = -1;
        return;
    }
    print_rga_ops_if_reordered(param, normalized_ops, normalized_count);

    int release_fence_fd = -1;
    for (int i = 0; i < normalized_count && i < 6; ++i)
    {
        // 注意这里不是“每个操作都立刻提交一次硬件任务”，
        // 而是在 CPU 侧先把最终状态推导出来，最后只调用一次 improcess()。
        switch (normalized_ops[i])
        {
        case RgaOpRotate:
            switch (rotation_to_usage(param->rotateAngle))
            {
            case IM_HAL_TRANSFORM_ROT_90:
                usage |= IM_HAL_TRANSFORM_ROT_90;
                {
                    // 90/270 度旋转会改变可见输出几何，因此需要交换当前宽高。
                    int tmp = out_w;
                    out_w = out_h;
                    out_h = tmp;
                }
                printf("旋转90°\n");
                break;
            case IM_HAL_TRANSFORM_ROT_180:
                usage |= IM_HAL_TRANSFORM_ROT_180;
                printf("旋转180°\n");
                break;
            case IM_HAL_TRANSFORM_ROT_270:
                usage |= IM_HAL_TRANSFORM_ROT_270;
                {
                    int tmp = out_w;
                    out_w = out_h;
                    out_h = tmp;
                }
                printf("旋转270°\n");
                break;

            default:
                printf("[RkRga] skip unsupported rotate angle: %d\n", param->rotateAngle);
                break;
            }
            break;

        case RgaOpScale:
            if (param->scaleRate > 0.0f && param->scaleRate != 1.0f)
            {
                // 缩放是基于“当前状态尺寸”继续推导的。
                // 如果前面已经裁剪，这里缩放的就是裁剪后的尺寸。
                out_w = static_cast<int>(out_w * param->scaleRate);
                out_h = static_cast<int>(out_h * param->scaleRate);
                if (out_w < 1)
                    out_w = 1;
                if (out_h < 1)
                    out_h = 1;
            }
            printf("缩放: scaleRate=%f, out=%dx%d\n",
                   param->scaleRate, out_w, out_h);
            break;

        case RgaOpCrop:
            printf("裁剪\n");
            if (param->cropRect.width <= 0 || param->cropRect.height <= 0)
            {
                fprintf(stderr, "[RkRga] invalid crop rect: [%d,%d,%d,%d]\n",
                        param->cropRect.x, param->cropRect.y,
                        param->cropRect.width, param->cropRect.height);
                result->fenceFd = -1;
                return;
            }
            src_crop_x = param->cropRect.x;
            src_crop_y = param->cropRect.y;
            src_crop_w = param->cropRect.width;
            src_crop_h = param->cropRect.height;
            // 裁剪后，当前可见输出尺寸直接重置为裁剪框大小。
            out_w = src_crop_w;
            out_h = src_crop_h;
            break;

        case RgaOpMirror:
            usage |= IM_HAL_TRANSFORM_FLIP_H;
            printf("镜像: horizontal flip\n");
            break;

        case RgaOpConvertColorSpace:
            printf("颜色空间转换: %s -> %s\n",
                   color_space_to_string(param->srcFormat),
                   color_space_to_string(param->dstFormat));
            break;
        case RgaOpMerge:
            fg_existed = true;
            // 计算合并区域的可见输出尺寸
            fg_bgX = param->merge.bgX;
            fg_bgY = param->merge.bgY;
            usage |= IM_ALPHA_BLEND_SRC_OVER;
            break;
        // case RgaOpConvertResolution:
        //     // 要求 目标分辨率必须小于等于源分辨率
        //     // 逻辑，现将目标分辨率先等比例放大接近源分辨率，再裁剪，最后在按等比例缩小
        //     // 计算出宽或高随


        //     printf("分辨率转换: center crop\n");
        //     if (param->resolution.width <= 0 || param->resolution.height <= 0)
        //     {
        //         fprintf(stderr, "[RkRga] invalid convert resolution rect: [%d,%d]\n",
        //                 param->resolution.width, param->resolution.height);
        //         result->fenceFd = -1;
        //         return;
        //     }
        //     src_crop_x = 0;
        //     src_crop_y = 0;
        //     src_crop_w = param->resolution.width;
        //     src_crop_h = param->resolution.height;
        //     // 当前实现里 ConvertResolution 本质上也是通过 crop 语义落地，
        //     // 所以它和 Crop 一样会重写源区域与当前输出尺寸。
        //     out_w = src_crop_w;
        //     out_h = src_crop_h;
        //     break;

        default:
            /* 未知操作码，后续可在这里补错误处理 */
            break;
        }
    }

    if (src_crop_x < 0 || src_crop_y < 0 || src_crop_w <= 0 || src_crop_h <= 0 ||
        src_crop_x + src_crop_w > param->srcWidth ||
        src_crop_y + src_crop_h > param->srcHeight)
    {
        fprintf(stderr,
                "[RkRga] crop rect [%d,%d,%d,%d] out of source bounds [%d,%d]\n",
                src_crop_x, src_crop_y, src_crop_w, src_crop_h,
                param->srcWidth, param->srcHeight);
        result->fenceFd = -1;
        return;
    }

    if (out_w <= 0 || out_h <= 0)
    {
        fprintf(stderr, "[RkRga] invalid output size: %dx%d\n", out_w, out_h);
        result->fenceFd = -1;
        return;
    }

    // 这里开始处理“内存布局”和“可见尺寸”的区别。
    //
    // out_w/out_h：
    //   业务上真正关心的可见输出尺寸。
    //
    // dst_wstride/dst_hstride：
    //   底层 buffer 的存储布局尺寸，通常会按格式要求向上对齐。
    //
    // 例如 RGB888 可见宽度是 641，但 stride 可能要按 4 对齐到 644。
    // 这并不表示图像真的变宽了，只是每行底层多留了一点 padding。
    int alignment = dimension_alignment(param->dstFormat);
    int dst_wstride = align_up(out_w, alignment);
    int dst_hstride = align_up(out_h, alignment);
    if (dst_wstride != out_w || dst_hstride != out_h)
    {
        fprintf(stderr,
                "[RkRga] align output stride for %s: visible=%dx%d stride=%dx%d, alignment=%d\n",
                color_space_to_string(param->dstFormat),
                out_w, out_h,
                dst_wstride, dst_hstride,
                alignment);
    }

    im_rect srect = {src_crop_x, src_crop_y, src_crop_w, src_crop_h};
    im_rect drect = {0, 0, out_w, out_h};
    if(fg_existed) {
        drect.x = fg_bgX;
        drect.y = fg_bgY;
    }
    im_rect prect = {0, 0, 0, 0};

    // size 按 stride 计算，而不是按可见 width/height 计算。
    // 这是因为真正分配 dma-buf 时，硬件访问的是底层布局，不是“理想紧密排布”。
    int dst_required_size = rk_rga_compute_bytes_of_format(param->dstFormat, dst_wstride, dst_hstride);

    if (param->dstDmaBufCapacity > 0 && dst_required_size > param->dstDmaBufCapacity)
    {
        fprintf(stderr,
                "[RkRga] dst buffer too small: required=%d capacity=%d, visible=%dx%d stride=%dx%d\n",
                dst_required_size, param->dstDmaBufCapacity,
                out_w, out_h, dst_wstride, dst_hstride);
        result->fenceFd = -1;
        return;
    }

    // fprintf(stdout,
    //         "[RkRga] improcess: src=%dx%d %s, srect=[%d,%d,%d,%d], dst=%dx%d %s, stride=%dx%d, drect=[%d,%d,%d,%d], usage=0x%x\n",
    //         param->srcWidth, param->srcHeight, color_space_to_string(param->srcFormat),
    //         srect.x, srect.y, srect.width, srect.height,
    //         out_w, out_h, color_space_to_string(param->dstFormat),
    //         dst_wstride, dst_hstride,
    //         drect.x, drect.y, drect.width, drect.height,
    //         usage);

    rga_buffer_t src = wrapbuffer_fd(task->param.srcDmaBufFd,
                                     task->param.srcWidth,
                                     task->param.srcHeight,
                                     src_rga_fmt);
    rga_buffer_t dst = wrapbuffer_fd(task->param.dstDmaBufFd,
                                     out_w,
                                     out_h,
                                     dst_rga_fmt,
                                     dst_wstride,
                                     dst_hstride);
    
    rga_buffer_t pat = {0};

    IM_STATUS check_ret = imcheck(src, dst, srect, drect, usage);
    if (check_ret != IM_STATUS_SUCCESS && check_ret != IM_STATUS_NOERROR)
    {
        fprintf(stderr, "[RkRga] imcheck error ret=%d, msg=%s\n",
                check_ret, imStrError(check_ret));
    }

  
    IM_STATUS ret = improcess(
        src,
        dst,
        pat,
        srect,
        drect,
        prect,
        param->parentFenceFd,
        &release_fence_fd,
        nullptr,
        usage);
    if (ret != IM_STATUS_SUCCESS)
    {
        printf("improcess error ret=%d, msg=%s\n", ret, imStrError(ret));
        result->fenceFd = -1;
        return;
    }

    printf("improcess run successful\n");

    // 最终回填时，把“可见尺寸”和“底层 stride”都带回去。
    // 上层如果要显示/喂模型，通常看 width/height；
    // 如果要正确解释 dma-buf 布局，则必须同时参考 strideWidth/strideHeight。
    result->width = out_w;
    result->height = out_h;
    result->strideWidth = dst_wstride;
    result->strideHeight = dst_hstride;
    result->size = dst_required_size;
    result->fenceFd = release_fence_fd;

   
}

// ============================================================
// 工作线程
// ============================================================

static void *rga_worker(void *arg)
{
    RkRgaInternal *ctx = static_cast<RkRgaInternal *>(arg);

    while (true)
    {
        pthread_mutex_lock(&ctx->mutex);
        while (!ctx->head && ctx->running)
        {
            pthread_cond_wait(&ctx->cond, &ctx->mutex);
        }
        if (!ctx->running && !ctx->head)
        {
            pthread_mutex_unlock(&ctx->mutex);
            break;
        }

        RgaTask *task = ctx->head;
        ctx->head = task->next;
        if (!ctx->head)
            ctx->tail = nullptr;
        pthread_mutex_unlock(&ctx->mutex);

        do_rga_task(task);
        free(task);
    }
    return nullptr;
}

// ============================================================
// 公共 API
// ============================================================

/**
 * 打印 RGA 提交任务的输入参数
 */
static void printRgaSubmitTaskInput(const RgaParam *param, RgaResult *result)
{
    printf("\n========== rk_rga_submit_task 输入参数 ==========\n");

    printf("[DMA 缓冲区信息]\n");
    printf("  parentFenceFd (父级 dma-fence FD): %d\n", param->parentFenceFd);
    printf("  srcDmaBufFd (源 DMA 缓冲区 FD): %d\n", param->srcDmaBufFd);
    printf("  srcDmaBufSize (源 DMA 缓冲区大小): %d 字节\n", param->srcDmaBufSize);
    printf("  dstDmaBufFd (目标 DMA 缓冲区 FD): %d\n", param->dstDmaBufFd);
    printf("  dstDmaBufCapacity (目标 DMA 缓冲区容量): %d 字节\n", param->dstDmaBufCapacity);

    printf("\n[源图像信息]\n");
    printf("  srcWidth (源图像宽度): %d\n", param->srcWidth);
    printf("  srcHeight (源图像高度): %d\n", param->srcHeight);
    printf("  srcFormat (源图像格式): %d (%s)\n", param->srcFormat, color_space_to_string(param->srcFormat));

    printf("\n[目标图像信息]\n");
    printf("  dstFormat (目标图像格式): %d (%s)\n", param->dstFormat, color_space_to_string(param->dstFormat));

    printf("\n[操作参数]\n");
    printf("  rotateAngle (旋转角度): %d 度\n", param->rotateAngle);
    printf("  scaleRate (缩放比例): %f\n", param->scaleRate);
    printf("  cropRect (裁剪区域) - x: %d, y: %d, width: %d, height: %d\n",
           param->cropRect.x, param->cropRect.y,
           param->cropRect.width, param->cropRect.height);
    // printf("  resolution (目标分辨率) - width: %d, height: %d\n",
    //        param->resolution.width, param->resolution.height);

    printf("\n[操作码信息]\n");
    printf("  opCount (操作数量): %d\n", param->opCount);
    for (int i = 0; i < param->opCount && i < 6; i++)
    {
        printf("  ops[%d]: %d (%s)\n", i, param->ops[i], rga_op_to_string(param->ops[i]));
    }

    printf("\n[结果指针]\n");
    printf("  result 指针: %p\n", (void *)result);
    if (result != nullptr)
    {
        printf("  result 当前值: width=%d height=%d strideWidth=%d strideHeight=%d size=%d fenceFd=%d\n",
               result->width,
               result->height,
               result->strideWidth,
               result->strideHeight,
               result->size,
               result->fenceFd);
    }
    printf("==========================================\n\n");
}

int rk_rga_init(RkRgaCtx *ctx)
{
    if (!ctx)
        return -1;

    auto *internal = static_cast<RkRgaInternal *>(
        calloc(1, sizeof(RkRgaInternal)));
    if (!internal)
        return -1;

    pthread_mutex_init(&internal->mutex, nullptr);
    pthread_cond_init(&internal->cond, nullptr);
    internal->running = 1;

    if (pthread_create(&internal->thread, nullptr, rga_worker, internal) != 0)
    {
        fprintf(stderr, "[RkRga] Failed to create worker thread\n");
        pthread_mutex_destroy(&internal->mutex);
        pthread_cond_destroy(&internal->cond);
        free(internal);
        return -1;
    }

    *ctx = static_cast<RkRgaCtx>(internal);
    printf("[RkRga] Initialized\n");
    return 0;
}

int rk_rga_release(RkRgaCtx ctx)
{
    if (!ctx)
        return -1;
    auto *internal = static_cast<RkRgaInternal *>(ctx);

    /* 信号通知工作线程停止 */
    pthread_mutex_lock(&internal->mutex);
    internal->running = 0;
    pthread_cond_signal(&internal->cond);
    pthread_mutex_unlock(&internal->mutex);

    pthread_join(internal->thread, nullptr);

    /* 排空任何剩余的任务 */
    RgaTask *t = internal->head;
    while (t)
    {
        RgaTask *next = t->next;
        t->result->fenceFd = -1;
        free(t);
        t = next;
    }

    pthread_mutex_destroy(&internal->mutex);
    pthread_cond_destroy(&internal->cond);
    free(internal);

    printf("[RkRga] Released\n");
    return 0;
}

int rk_rga_submit_task(RkRgaCtx ctx, const RgaParam *param, RgaResult *result)
{
    if (!ctx || !param || !result)
        return -1;

    /* 打印输入参数 */
    printRgaSubmitTaskInput(param, result);

    /* 同步执行：调用方通常在采集/渲染线程中传入栈上 result，
     * 返回前必须完成填充，避免后台线程写悬空地址。 */
    RgaTask task;
    task.param = *param;
    task.result = result;
    task.next = nullptr;

    result->fenceFd = -1;
    result->width = 0;
    result->height = 0;
    result->strideWidth = 0;
    result->strideHeight = 0;
    result->size = 0;

    do_rga_task(&task);
    return 0;
}
