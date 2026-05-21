#ifndef RK_HW_IMAGE_TYPES_H
#define RK_HW_IMAGE_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ColorSpace_NONE = 0,
    ColorSpace_RGB = 1 << 1,
    ColorSpace_BGR = 1 << 2,
    ColorSpace_NV12 = 1 << 3,
    ColorSpace_GRAY = 1 << 4,
    ColorSpace_YUV420P = 1 << 5,
    ColorSpace_MJPEG = 1 << 6,
    ColorSpace_YUYV = 1 << 7,
    ColorSpace_RGBA = 1 << 8,
    ColorSpace_H264 = 1 << 9,
} ColorSpaceEnum;

typedef enum {
    ImageRotation_0 = 0,
    ImageRotation_90 = 90,
    ImageRotation_180 = 180,
    ImageRotation_270 = 270,
} ImageRotationEnum;

typedef enum {
    ImageMirror_None = 1 << 0,
    ImageMirror_Horizontal = 1 << 1,
    ImageMirror_Vertical = 1 << 2,
    ImageMirror_Horizontal_Vertical = 1 << 3,
} ImageMirrorEnum;

typedef struct {
    int x;
    int y;
    int width;
    int height;
} ImageRect;

typedef struct {
    int bgX;
    int bgY;
} ImageMerge;

#ifdef __cplusplus
}
#endif

#endif
