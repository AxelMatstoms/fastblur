#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include <argp.h>

#include "stb_image.h"
#include "stb_image_write.h"

#define GAMMA 2.2f

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

#define IDX(w, x, y, s) ((s) * ((w) * (y) + (x)))
#define PTR_SWAP(a, b) { \
    void *tmp = a; \
    a = b; \
    b = tmp; }

#ifdef MEASURE_PERF_ENABLE
#define TIMER_START(name) clock_t _timer_ ## name = clock()
#define TIMER_END(name) \
    { clock_t _timer_end_ = clock(); \
        float _timer_duration_ms_ = 1000.0 * (_timer_end_ - _timer_ ## name) / CLOCKS_PER_SEC; \
        printf(#name ": %.2fms\n", _timer_duration_ms_); }
#else
#define TIMER_START(name) (void) 0
#define TIMER_END(name) (void) 0
#endif /* MEASURE_PERF_ENABLE */

struct img {
    int width;
    int height;
    int stride;
    bool owner;
    size_t alloc_size;
    float *pixels;
};

struct geometry {
    int width;
    int height;
    float anchor;
};

enum crop_mode {
    CROP_NONE,
    CROP_FILL
};

enum pixel_format {
    FORMAT_RGB,
    FORMAT_RGBA,
    FORMAT_ARGB,
    FORMAT_BGR,
    FORMAT_BGRA,
    FORMAT_ABGR,

    FORMAT_COUNT
};

struct raw_image_format {
    enum pixel_format format;
    int width;
    int height;
};

struct arguments {
    char *output_file;
    char *input_file;
    bool fast_gamma;
    bool raw_image;
    unsigned blur_size;
    unsigned blur_passes;
    enum crop_mode crop_mode;
    struct geometry geom;
    struct raw_image_format raw_fmt;
};

static const int pixel_format_size[FORMAT_COUNT] = {
    [FORMAT_RGB]  = 3,
    [FORMAT_RGBA] = 4,
    [FORMAT_ARGB] = 4,
    [FORMAT_BGR]  = 3,
    [FORMAT_BGRA] = 4,
    [FORMAT_ABGR] = 4
};

static const int pixel_format_rgb_offset[FORMAT_COUNT][3] = {
    [FORMAT_RGB]  = {0, 1, 2},
    [FORMAT_RGBA] = {0, 1, 2},
    [FORMAT_ARGB] = {1, 2, 3},
    [FORMAT_BGR]  = {2, 1, 0},
    [FORMAT_BGRA] = {2, 1, 0},
    [FORMAT_ABGR] = {3, 2, 1}
};


float gamma_decode_lut[256];

const char *argp_program_version =
    "fastblur 0.2.0";

static char doc[] =
    "fastblur -- quickly blur images with efficient filtering";

static char args_doc[] = "FILE";

char *program_name;

void fmt_error_and_exit(const char *format, ...)
{
    char err[256];
    va_list args;
    va_start(args, format);
    vsnprintf(err, 256, format, args);
    va_end(args);

    fprintf(stderr, "%s: %s\n", program_name, err);

    exit(EXIT_FAILURE);
}

/**
 * Set the size of an image.
 *
 * Data may or may not be preserved. If the image already fits the
 * requested size, no allocations are made.
 */
void img_set_size(struct img *img, int width, int height)
{
    img->width = width;
    img->height = height;
    int size = sizeof(float) * 3 * img->width * img->height;
    img->stride = 3 * width;
    if (!img->pixels || size > img->alloc_size) {
        free(img->pixels);
        img->alloc_size = size;
        img->pixels = malloc(sizeof(float) * img->alloc_size * 3);
    }
}

/**
 * Initialize an image to a size.
 *
 * img_set_size requires the image to be in a valid state. img_init sets
 * the image to a valid state and then calls img_set_size.
 */
void img_init(struct img *img, int w, int h)
{
    img->pixels = NULL;
    img_set_size(img, w, h);
}

/**
 * Initialize the gamma decode lut.
 *
 * powf is slow. Use a lut to improve performance.
 */
void init_gamma_decode_lut()
{
    const float scale_factor = 1.0f / 255.0f;

    for (size_t i = 0; i < 256; i++) {
        gamma_decode_lut[i] = powf(i * scale_factor, GAMMA);
    }
}

/**
 * Decode gamma-encoded values.
 *
 * Fast gamma is an approximation that uses gamma=2.0 instead of the
 * usual 2.2 for sRGB. Fast gamma improves encoding performance since
 * there is no fast way to use a lut with floats.
 */
float gamma_decode_fast(uint8_t v)
{
    static const float scale_factor = 1.0f / 255.0f;

    float x = v * scale_factor;
    return x * x;
}

/**
 * Gamma-encode a linear value.
 */
uint8_t gamma_encode(float v)
{
    static const float gamma_rcp = 1.0f / GAMMA;

    return (uint8_t) (255.0f * powf(v, gamma_rcp) + 0.5f);
}

/**
 * Gamma-encode a linear value.
 *
 * Uses the fast gamma approximation, which uses a call to sqrtf
 * instead of powf.
 */
uint8_t gamma_encode_fast(float v)
{
    return (uint8_t) (255.0f * sqrtf(v) + 0.5f);
}

void img_gamma_decode_bitmap(struct img *img, uint8_t *bitmap,
        struct raw_image_format *fmt, bool fast_gamma)
{
    img_init(img, fmt->width, fmt->height);

    int pixel_size = pixel_format_size[fmt->format];
    int img_size = pixel_size * fmt->width * fmt->height;
    const int *offset = pixel_format_rgb_offset[fmt->format];

    for (size_t i = 0; i < img_size; i += pixel_size) {
        for (size_t c = 0; c < 3; c++) {
            if (fast_gamma) {
                img->pixels[i + c] = gamma_decode_fast(bitmap[i + offset[c]]);
            } else {
                img->pixels[i + c] = gamma_decode_lut[bitmap[i + offset[c]]];
            }
        }
    }
}

uint8_t *img_gamma_encode_to_bitmap(struct img *img, bool fast_gamma)
{
    size_t size = 3 * img->width * img->height;
    uint8_t *bitmap = malloc(size);

    for (int y = 0; y < img->height; y++) {
        float *row = &img->pixels[img->stride * y];
        uint8_t *out_row = &bitmap[3 * img->width * y];
        for (int i = 0; i < 3 * img->width; i++) {
            if (fast_gamma) {
                out_row[i] = gamma_encode_fast(row[i]);
            } else {
                out_row[i] = gamma_encode(row[i]);
            }
        }
    }

    return bitmap;
}

/**
 * Load an image from a file and convert to linear colors.
 */
void img_load(struct img *img, char *pathname, bool fast_gamma)
{
    int width, height, channels;
    bool use_stdin = (strcmp(pathname, "-") == 0);

    uint8_t *bitmap;
    if (use_stdin) {
        bitmap = stbi_load_from_file(stdin, &width, &height, &channels, 3);
    } else {
        bitmap = stbi_load(pathname, &width, &height, &channels, 3);
    }

    if (!bitmap) {
        fmt_error_and_exit("could not load image from %s", pathname);
    }

    struct raw_image_format format = { FORMAT_RGB, width, height };
    img_gamma_decode_bitmap(img, bitmap, &format, fast_gamma);

    free(bitmap);
}

void img_load_raw(struct img *img, char *pathname,
        struct raw_image_format *raw_fmt, bool fast_gamma)
{
    int pixel_size = pixel_format_size[raw_fmt->format];
    size_t raw_img_size = pixel_size * raw_fmt->width * raw_fmt->height;

    bool use_stdin = (strcmp(pathname, "-") == 0);
    FILE *file = use_stdin ? stdin : fopen(pathname, "r");
    if (!file) {
        int errsv = errno;
        char *err = strerror(errsv);
        fmt_error_and_exit("cannot open '%s' (%s)", pathname, err);
    }

    uint8_t *bitmap = malloc(raw_img_size);

    size_t bytes_read = fread(bitmap, 1, raw_img_size, file);
    if (bytes_read != raw_img_size) {
        fmt_error_and_exit("unexpected eof before raw image end");
    }

    if (use_stdin) {
        fclose(file);
    }

    img_gamma_decode_bitmap(img, bitmap, raw_fmt, fast_gamma);
    free(bitmap);
}

/**
 * Gamma-encode an image and save it.
 */
void img_save_png(struct img *img, char *pathname, bool fast_gamma)
{
    uint8_t *bitmap = img_gamma_encode_to_bitmap(img, fast_gamma);

    int stride = 3 * img->width;
    stbi_write_png(pathname, img->width, img->height, 3, bitmap, stride);
    free(bitmap);
}

/**
 * Transpose an image.
 *
 * Since images are stored in row-major order, operations working in
 * row-major order perform about 3x better than operations that work
 * in column-major order. If many column-major operations are performed
 * consecutively it may be faster to transpose the image before and
 * after.
 */
void img_transpose(struct img *src, struct img *dst)
{
    img_set_size(dst, src->height, src->width);

    size_t src_w = src->width;
    size_t src_h = src->height;

    for (size_t y = 0; y < src_h; y++) {
        for (size_t x = 0; x < src_w; x++) {
            for (size_t c = 0; c < 3; c++) {
                dst->pixels[IDX(src_h, y, x, 3) + c]
                    = src->pixels[IDX(src_w, x, y, 3) + c];
            }
        }
    }
}

/**
 * Creates a cropped view of an image.
 *
 * The cropped image points to the same data, but has a changed width
 * and height, but not stride. The pointer points to the first pixel
 * in the cropped image.
 */
struct img img_crop(struct img *src, int w, int h, int x, int y)
{
    return (struct img) { w, h, src->stride, false, 0,
            &src->pixels[y * src->stride + 3 * x] };
}

/**
 * Perform nearest neigbor scaling.
 */
struct img img_interp_nearest(struct img *src, int width, int height)
{
    const float dst_width_rcp = 1.0f / width;
    const float dst_height_rcp = 1.0f / height;

    struct img dst;
    img_init(&dst, width, height);

    for (int y = 0; y < dst.height; y++) {
        float (*dst_row)[3] = (float (*)[3]) &dst.pixels[dst.stride * y];

        int y_src = y * src->height * dst_height_rcp + 0.5;
        float (*src_row)[3] = (float (*)[3]) &src->pixels[src->stride * y_src];

        for (int x = 0; x < dst.width; x++) {
            int x_src = x * src->width * dst_width_rcp + 0.5;

            for (int c = 0; c < 3; c++) {
                dst_row[x][c] = src_row[x_src][c];
            }
        }
    }

    return dst;
}

void img_box2x2(struct img *src, struct img *dst)
{
    img_set_size(dst, src->width / 2, src->height / 2);
    for (int y = 0; y < dst->height; y++) {
        float (*dst_row)[3] = (float (*)[3]) &dst->pixels[dst->stride * y];
        float (*src_row)[3] = (float (*)[3]) &src->pixels[src->stride * y * 2];
        float (*src_row2)[3] = (float (*)[3]) &src->pixels[src->stride * y * 2 + 1];

        for (int x = 0; x < dst->height; x++) {
            for (int c = 0; c < 3; c++) {
                float sum = 0.0f;
                sum += src_row[2 * x][c];
                sum += src_row[2 * x + 1][c];
                sum += src_row2[2 * x][c];
                sum += src_row2[2 * x + 1][c];
                dst_row[x][c] = 0.25f * sum;
            }
        }
    }
}

void img_decimate(struct img *img, int n)
{
    struct img tmp;
    tmp.pixels = NULL;
    struct img *dst = &tmp;
    struct img *src = img;


    for (int i = 0; i < n; i++) {
        img_box2x2(src, dst);
        PTR_SWAP(src, dst);
    }

    free(dst->pixels);
    *img = *src;
}

void img_resize_fill(struct img *img, struct geometry *geom)
{
    float crop_aspect_ratio = (float) geom->width / geom->height;
    float img_aspect_ratio = (float) img->width / img->height;

    int crop_w = img->width;
    int crop_h = img->height;
    int crop_x = 0;
    int crop_y = 0;

    if (crop_aspect_ratio > img_aspect_ratio) {
        crop_h = (int) (img->width / crop_aspect_ratio + 0.5);
        crop_y = (int) (geom->anchor * (img->height - crop_h) + 0.5);
    } else {
        crop_w = (int) (img->height * crop_aspect_ratio + 0.5);
        crop_x = (int) (geom->anchor * (img->width - crop_w) + 0.5);
    }

    float scale_factor = (float) crop_w / (float) geom->width;

    struct img cropped = img_crop(img, crop_w, crop_h, crop_x, crop_y);

    struct img resized = img_interp_nearest(&cropped, geom->width, geom->height);

    free(img->pixels);
    *img = resized;
}

/**
 * Apply a recursive moving average filter horizontally.
 *
 * The recursive implementation is O(h * (w + n)) instead of
 * O(w * w * n) for convolution. This improves performance drastically,
 * especially for large values of n.
 */
void img_mov_avg_h(struct img *src, struct img *dst, int n)
{
    img_set_size(dst, src->width, src->height);

    int w = src->width;
    int h = src->height;

    float a = 1.0f / n;
    int p = (n - 1) / 2;
    int q = p + 1;

    for (int y = 0; y < h; y++) {
        // Simplify indexing by casting to pointer to float[3]
        float (*src_row)[3] = (float (*)[3]) &src->pixels[src->stride * y];
        float (*dst_row)[3] = (float (*)[3]) &dst->pixels[dst->stride * y];

        // Compute first value using convolution. Since the edges are
        // clamped, the left half is just multiplication.
        for (int c = 0; c < 3; c++) {
            dst_row[0][c] = src_row[0][c] * q * a;
        }

        for (int x = 1; x < q; x++) {
            for (int c = 0; c < 3; c++) {
                dst_row[0][c] += a * src_row[x][c];
            }
        }

        // Calculate remaining pixels recursively.
        // y[n] = x[n - p] + ... + x[n + p] <=>
        // y[n] = y[n - 1] + x[n + p] - x[n - q]
        for (int x = 1; x < w; x++) {
            for (int c = 0; c < 3; c++) {
                dst_row[x][c] = dst_row[x - 1][c]
                    + a * src_row[MIN(x + p, w - 1)][c]
                    - a * src_row[MAX(x - q, 0)][c];
            }
        }
    }
}

/*
 * Apply a recursive moving average filter vertically.
 *
 * The recursive implementation is O(h * (w + n)) instead of
 * O(w * w * n) for convolution. This improves performance drastically,
 * especially for large values of n.
 *
 * Due to images being stored in row-major order this is about 3x slower
 * than img_mov_avg_h.
 */
void img_mov_avg_v(struct img *src, struct img *dst, int n)
{
    img_set_size(dst, src->width, src->height);

    int w = src->width;
    int h = src->height;

    float a = 1.0f / n;
    int p = (n - 1) / 2;
    int q = p + 1;

    for (int x = 0; x < w; x++) {
        float *src_col = src->pixels + 3 * x;
        float *dst_col = dst->pixels + 3 * x;

        // Compute first value using convolution. Since the edges are
        // clamped, the left half is just multiplication.
        for (int c = 0; c < 3; c++) {
            dst_col[c] = src_col[c] * q * a;
        }

        for (int y = 1; y < q; y++) {
            for (int c = 0; c < 3; c++) {
                dst_col[c] += a * src_col[y * src->stride + c];
            }
        }

        // Calculate remaining pixels recursively.
        // y[n] = x[n - p] + ... + x[n + p] <=>
        // y[n] = y[n - 1] + x[n + p] - x[n - q]
        for (int y = 1; y < h; y++) {
            for (int c = 0; c < 3; c++) {
                dst_col[y * dst->stride + c] = dst_col[(y - 1) * dst->stride + c]
                    + a * src_col[MIN(y + p, h - 1) * src->stride + c]
                    - a * src_col[MAX(y - q, 0) * src->stride + c];
            }
        }
    }
}

int parse_geometry(char *str, struct geometry *geom)
{
    char *ptr;
    geom->width = strtol(str, &ptr, 10);

    if (ptr == str || *ptr != 'x')
        return 0;

    str = ptr;
    str++; // Skip 'x'

    geom->height = strtol(str, &ptr, 10);

    if (ptr == str)
        return 0;

    str = ptr;
    if (*str != '@')
        return 1;

    str++;
    geom->anchor = strtof(str, &ptr);

    return !(ptr == str);
}

int parse_raw_format(char *str, struct raw_image_format *raw_fmt)
{
    char *ptr;
    raw_fmt->width = strtol(str, &ptr, 10);

    if (ptr == str || *ptr != 'x')
        return 0;

    str = ptr;
    str++;

    raw_fmt->height = strtol(str, &ptr, 10);

    if (ptr == str || *ptr != ':')
        return 0;

    str = ptr;
    str++;

    bool alpha_first = (*str == 'a');
    if (alpha_first)
        str++;

    bool rgb = strncmp(str, "rgb", 3);
    bool bgr = strncmp(str, "bgr", 3);

    if (!rgb && !bgr)
        return 0;

    bool alpha_last = !alpha_first && (str[3] == 'a');
    if (alpha_first) {
        raw_fmt->format = rgb ? FORMAT_ARGB : FORMAT_ABGR;
    } else if (alpha_last) {
        raw_fmt->format = rgb ? FORMAT_RGBA : FORMAT_BGRA;
    } else {
        raw_fmt->format = rgb ? FORMAT_RGB : FORMAT_BGR;
    }

    return 1;
}

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    struct arguments *arguments = state->input;
    switch (key) {
        case 'G':
            arguments->fast_gamma = true;
            break;
        case 'z':
            {
                char *end;
                int size = strtol(arg, &end, 10);
                if (end == arg || size < 1)
                    argp_error(state, "invalid size, must be at least 1.");
                if (size % 2 == 0)
                    argp_error(state, "invalid size, must be odd");

                arguments->blur_size = size;
            }
            break;
        case 'p':
            {
                char *end;
                int passes = strtoul(arg, &end, 10);
                if (end == arg || passes < 1)
                    argp_error(state, "invalid count, must be at least 1.");

                arguments->blur_passes = passes;
            }
            break;
        case 'r':
            arguments->crop_mode = CROP_FILL;
            if (!parse_geometry(arg, &arguments->geom)) {
                argp_error(state, "invalid geometry, format WxH@A.");
            }
            break;
        case 0x100:
            if (!parse_raw_format(arg, &arguments->raw_fmt)) {
                argp_error(state, "invalid raw image format, WxH:FORMAT.");
            }
            arguments->raw_image = true;
            break;
        case ARGP_KEY_ARG:
            if (state->arg_num == 0) {
                arguments->input_file = arg;
            } else if (state->arg_num == 1) {
                arguments->output_file = arg;
            } else {
                argp_usage(state);
            }
            break;
        case ARGP_KEY_END:
            if (state->arg_num < 1)
                argp_usage(state);
            break;
        default:
            return ARGP_ERR_UNKNOWN;
    }

    return 0;
}

int main(int argc, char **argv)
{
    program_name = argv[0];

    const struct argp_option options[] = {
        {"fast-gamma",  'G',   0,          0, "Use fast, less accurate gamma" },
        {"blur-size",   'z',   "SIZE",     0,
         "Use a moving average filter of length SIZE" },
        {"blur-passes", 'p',   "COUNT",    0, "Do COUNT filter passes" },
        {"resize",      'r',   "GEOMETRY", 0,
         "Resize the input image before blurring" },
        {"raw",         0x100, "FORMAT",   0, "Read raw bitmap image" },
        { 0 }
    };

    struct argp argp = {options, parse_opt, args_doc, doc};

    struct arguments arguments;
    arguments.fast_gamma = false;
    arguments.raw_image = false;
    arguments.blur_size = 31;
    arguments.blur_passes = 4;
    arguments.crop_mode = CROP_NONE;
    arguments.geom = (struct geometry) {-1, -1, 0.5};

    argp_parse(&argp, argc, argv, 0, 0, &arguments);

    int passes = arguments.blur_passes;
    int blur_size = arguments.blur_size;

    if (!arguments.fast_gamma)
        init_gamma_decode_lut();

    struct img img;
    if (arguments.raw_image) {
        img_load_raw(&img, arguments.input_file, &arguments.raw_fmt, arguments.fast_gamma);
    } else {
        img_load(&img, arguments.input_file, arguments.fast_gamma);
    }

    if (arguments.crop_mode == CROP_FILL) {
        TIMER_START(resize);

        float crop_aspect_ratio = (float) arguments.geom.width / arguments.geom.height;
        float img_aspect_ratio = (float) img.width / img.height;

        int crop_w = img.width;
        int crop_h = img.height;
        int crop_x = 0;
        int crop_y = 0;

        if (crop_aspect_ratio > img_aspect_ratio) {
            crop_h = (int) (img.width / crop_aspect_ratio + 0.5);
            crop_y = (int) (arguments.geom.anchor * (img.height - crop_h) + 0.5);
        } else {
            crop_w = (int) (img.height * crop_aspect_ratio + 0.5);
            crop_x = (int) (arguments.geom.anchor * (img.width - crop_w) + 0.5);
        }

        struct img cropped = img_crop(&img, crop_w, crop_h, crop_x, crop_y);
        struct img resized = img_interp_nearest(&cropped,
            arguments.geom.width, arguments.geom.height);

        free(img.pixels);
        img = resized;

        TIMER_END(resize);
    }

    struct img img2;
    img_init(&img2, img.width, img.height);

    struct img *src = &img;
    struct img *dst = &img2;

    TIMER_START(hblur);
    for (int i = 0; i < passes; i++) {
        img_mov_avg_h(src, dst, blur_size);
        PTR_SWAP(src, dst);
    }
    TIMER_END(hblur);

    img_transpose(src, dst);
    PTR_SWAP(src, dst);

    TIMER_START(vblur);
    for (int i = 0; i < passes; i++) {
        img_mov_avg_h(src, dst, blur_size);
        PTR_SWAP(src, dst)
    }
    TIMER_END(vblur);

    img_transpose(src, dst);

    img_save_png(dst, arguments.output_file, arguments.fast_gamma);
}
