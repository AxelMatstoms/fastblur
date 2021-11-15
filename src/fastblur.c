#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#include <time.h>

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

float gamma_decode_lut[256];



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

float gamma_decode(uint8_t v)
{
#ifdef FAST_GAMMA
    static const float scale_factor = 1.0f / 255.0f;

    float x = v * scale_factor;
    return x * x;
#else
    return gamma_decode_lut[v];
#endif /* FAST_GAMMA */
}

uint8_t gamma_encode(float v)
{
#ifdef FAST_GAMMA
    return (uint8_t) (255.0f * sqrtf(v) + 0.5f);
#else
    return (uint8_t) (255.0f * powf(v, 1.0f / GAMMA) + 0.5f);
#endif /* FAST_GAMMA */
}

void img_load(struct img *img, char *pathname)
{
    img->size = 0;
    img->pixels = NULL;

    int channels;
    uint8_t *pixels = stbi_load(pathname, &img->width, &img->height, &channels, 3);

    img_alloc(img);
    int size = 3 * img->width * img->height;

    clock_t start = clock();
    for (size_t i = 0; i < size; i++) {
        img->pixels[i] = gamma_decode(pixels[i]);
    }
    clock_t end = clock();
    double ms = (end - start) * 1000.0 / CLOCKS_PER_SEC;;
    printf("gamma decoding took: %.1fms\n", ms);

    free(pixels);
}

void img_save_png(struct img *img, char *pathname)
{
    size_t size = 3 * img->width * img->height;
    uint8_t *pixels = malloc(sizeof(uint8_t) * size);

    clock_t start = clock();

    for (size_t i = 0; i < size; i++) {
        pixels[i] = gamma_encode(img->pixels[i]);
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

int main(int argc, char **argv)
{
    int blur_size = 51;
    int passes = 4;
    if (argc != 2) {
        return 0;
    }

#ifndef FAST_GAMMA
    init_gamma_decode_lut();
#endif /* FAST_GAMMA */

    struct img img;
    img_load(&img, argv[1]);

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

    img_save_png(dst, "out.png");
}
