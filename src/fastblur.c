#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

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

struct img {
    int width;
    int height;
    int size;
    float *pixels;
};

struct arguments {
    char *output_file;
    char *input_file;
    bool fast_gamma;
    unsigned blur_size;
    unsigned blur_passes;
};

float gamma_decode_lut[256];

const char *argp_program_version =
    "fastblur 0.1.0";

static char doc[] =
    "fastblur -- quickly blur images with efficient filtering";

static char args_doc[] = "FILE";

void img_alloc(struct img *img)
{
    int size = img->width * img->height;
    if (size > img->size) {
        free(img->pixels);
        img->size = size;
        img->pixels = malloc(sizeof(float) * img->size * 3);
    }
}

void img_init(struct img *img, int w, int h)
{
    img->width = w;
    img->height = h;
    img->size = 0;
    img->pixels = NULL;
    img_alloc(img);
}

void init_gamma_decode_lut()
{
    const float scale_factor = 1.0f / 255.0f;

    for (size_t i = 0; i < 256; i++) {
        gamma_decode_lut[i] = powf(i * scale_factor, GAMMA);
    }
}

float fast_gamma_decode(uint8_t v)
{
    static const float scale_factor = 1.0f / 255.0f;

    float x = v * scale_factor;
    return x * x;
}

uint8_t gamma_encode(float v)
{
    return (uint8_t) (255.0f * powf(v, 1.0f / GAMMA) + 0.5f);
}

uint8_t fast_gamma_encode(float v)
{
    return (uint8_t) (255.0f * sqrtf(v) + 0.5f);
}

void img_load(struct img *img, char *pathname, bool fast_gamma)
{
    img->size = 0;
    img->pixels = NULL;

    int channels;
    uint8_t *pixels = stbi_load(pathname, &img->width, &img->height, &channels, 3);

    if (!pixels) {
        fprintf(stderr, "fastblur: cannot load image from '%s'.\n", pathname);
        exit(1);
    }

    img_alloc(img);
    int size = 3 * img->width * img->height;

    clock_t start = clock();
    for (size_t i = 0; i < size; i++) {
        if (fast_gamma) {
            img->pixels[i] = fast_gamma_decode(pixels[i]);
        } else {
            img->pixels[i] = gamma_decode_lut[pixels[i]];
        }
    }
    clock_t end = clock();
    double ms = (end - start) * 1000.0 / CLOCKS_PER_SEC;;
    printf("gamma decoding took: %.1fms\n", ms);

    free(pixels);
}

void img_save_png(struct img *img, char *pathname, bool fast_gamma)
{
    size_t size = 3 * img->width * img->height;
    uint8_t *pixels = malloc(sizeof(uint8_t) * size);

    clock_t start = clock();

    for (size_t i = 0; i < size; i++) {
        if (fast_gamma) {
            pixels[i] = fast_gamma_encode(img->pixels[i]);
        } else {
            pixels[i] = gamma_encode(img->pixels[i]);
        }
    }

    clock_t end = clock();
    double ms = (end - start) * 1000.0 / CLOCKS_PER_SEC;
    printf("gamma encoding took: %.1fms\n", ms);

    int stride = 3 * img->width;
    stbi_write_png(pathname, img->width, img->height, 3, pixels, stride);
    free(pixels);
}


void img_transpose(struct img *src, struct img *dst)
{
    dst->width = src->height;
    dst->height = src->width;
    img_alloc(dst);

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

void img_mov_avg_h(struct img *src, struct img *dst, int n)
{
    dst->width = src->width;
    dst->height = src->height;
    img_alloc(dst);

    int w = src->width;
    int h = src->height;

    float a = 1.0f / n;
    int p = (n - 1) / 2;
    int q = p + 1;

    for (int y = 0; y < h; y++) {
        // Simplify indexing by casting to pointer to float[3]
        float (*src_row)[3] = (float (*)[3]) &src->pixels[3 * w * y];
        float (*dst_row)[3] = (float (*)[3]) &dst->pixels[3 * w * y];

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

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    struct arguments *arguments = state->input;

    switch (key) {
        case 'o':
            arguments->output_file = arg;
            break;
        case 'G':
            arguments->fast_gamma = true;
            break;
        case 'b':
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
        case ARGP_KEY_ARG:
            if (state->arg_num >= 1)
                argp_usage(state);
            arguments->input_file = arg;
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
    const struct argp_option options[] = {
        {"output",      'o', "FILE",  0, "Output to FILE" },
        {"fast-gamma",  'G', 0,       0, "Use fast, less accurate gamma" },
        {"blur-size",   'b', "SIZE",  0,
         "Use a moving average filter of length SIZE" },
        {"blur-passes", 'p', "COUNT", 0, "Do COUNT filter passes" },
        { 0 }
    };

    struct argp argp = {options, parse_opt, args_doc, doc};

    struct arguments arguments;
    arguments.output_file = "out.png";
    arguments.fast_gamma = false;
    arguments.blur_size = 31;
    arguments.blur_passes = 4;

    argp_parse(&argp, argc, argv, 0, 0, &arguments);

    int passes = arguments.blur_passes;
    int blur_size = arguments.blur_size;

    if (!arguments.fast_gamma)
        init_gamma_decode_lut();

    struct img img;
    img_load(&img, arguments.input_file, arguments.fast_gamma);

    struct img img2 = { .width = img.width, .height = img.height };
    img_alloc(&img2);

    struct img *src = &img;
    struct img *dst = &img2;

    for (int i = 0; i < passes - 1; i++) {
        img_mov_avg_h(src, dst, blur_size);
        PTR_SWAP(src, dst);
    }
    img_mov_avg_h(src, dst, blur_size);

    img_transpose(dst, src);

    for (int i = 0; i < passes; i++) {
        img_mov_avg_h(src, dst, blur_size);
        PTR_SWAP(src, dst)
    }

    img_transpose(src, dst);

    img_save_png(dst, arguments.output_file, arguments.fast_gamma);
}
