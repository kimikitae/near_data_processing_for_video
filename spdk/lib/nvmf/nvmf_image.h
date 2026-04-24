#ifndef NVMF_IMAGE_H
#define NVMF_IMAGE_H

#include <stdlib.h>

typedef struct {
    int w;
    int h;
    int c;
    float *data;
} image;

image make_empty_image(int w, int h, int c);
image make_image(int w, int h, int c);
image copy_image(image p);
void free_image(image m);
void fill_image(image m, float s);
void embed_image(image source, image dest, int dx, int dy);
image resize_image(image im, int w, int h);
image letterbox_image(image im, int w, int h);

#endif