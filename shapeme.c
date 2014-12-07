/*
 * Copyright (c) 2008-2014, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#define PNG_DEBUG 3
#include <png.h>
#include <SDL.h>

#define TYPE_TRIANGLE 0
#define TYPE_CIRCLE 1

#define MINALPHA 10
#define MAXALPHA 90

/* Configurable options. */
int opt_use_triangles = 1;
int opt_use_circles = 0;
int opt_restart = 0;
int opt_mutation_rate = 200;

/* The global state defines the global state we save and restore
 * in addition to the best candidate. */
struct globalState {
    int max_shapes; /* Total number of triangles + circles. */
    int max_shapes_incremental;
    float temperature, absbestdiff;
    long long generation;
} state;

/* Internally we represent our set of trinagels as an array of the triangle
 * structures. While the structure is named "trinalge" if type is TYPE_CIRCLE
 * it actually represents a circle. */
struct triangle {
    int type;
    unsigned char r,g,b,alpha;
    union {
        struct {
            short x1,y1,x2,y2,x3,y3;
        } t;
        struct {
            short x1,y1,radius;
        } c;
    } u;
};

struct triangles {
    struct triangle *triangles;
    int count;
    int inuse;
};

/* SDL initialization function. */
static SDL_Surface *sdlInit(int width, int height, int fullscreen) {
    int flags = SDL_SWSURFACE;
    SDL_Surface *screen;

    if (fullscreen) flags |= SDL_FULLSCREEN;
    if (SDL_Init(SDL_INIT_VIDEO) == -1) {
        fprintf(stderr, "SDL Init error: %s\n", SDL_GetError());
        return NULL;
    }
    atexit(SDL_Quit);
    screen = SDL_SetVideoMode(width,height,24,flags);
    if (!screen) {
        fprintf(stderr, "Can't set the video mode: %s\n", SDL_GetError());
        return NULL;
    }
    return screen;
}

/* Show a raw RGB image on the SDL screen. */
static void sdlShowRgb(SDL_Surface *screen, unsigned char *fb, int width,
        int height)
{
    unsigned char *s, *p = fb;
    int y,x;

    for (y = 0; y < height; y++) {
        s = screen->pixels+y*(screen->pitch);
        for (x = 0; x < width; x++) {
            s[0] = p[2];
            s[1] = p[1];
            s[2] = p[0];
            s += 3;
            p += 3;
        }
    }
    SDL_UpdateRect(screen, 0, 0, width-1, height-1);
}

/* Minimal SDL event processing, just a few keys to exit the program. */
static void processSdlEvents(void) {
    SDL_Event event;

    while(SDL_PollEvent(&event)) {
        switch(event.type) {
        case SDL_KEYDOWN:
            switch(event.key.keysym.sym) {
            case SDLK_q:
            case SDLK_ESCAPE:
                exit(0);
                break;
            default: break;
            }
        }
    }
}

/* Write a PNG file. The image is passed with row_pointers as an RGB image. */
int PngWrite(FILE *fp, int width, int height, png_bytep *row_pointers)
{
    png_structp png_ptr;
    png_infop info_ptr;
    int bit_depth = 8;

    /* Initialization */
    png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                    NULL, NULL, NULL);
    if (!png_ptr) return 1;
    info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) return 1;
    if (setjmp(png_jmpbuf(png_ptr))) return 1;
    png_init_io(png_ptr, fp);

    /* Write the header */
    if (setjmp(png_jmpbuf(png_ptr))) return 1;
    png_set_IHDR(png_ptr, info_ptr, width, height,
                 bit_depth, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_write_info(png_ptr, info_ptr);

    /* Write data */
    if (setjmp(png_jmpbuf(png_ptr))) return 1;
    png_write_image(png_ptr, row_pointers);

    /* End */
    if (setjmp(png_jmpbuf(png_ptr))) return 1;
    png_write_end(png_ptr, NULL);
    return 0;
}

/* Load a PNG and returns it as a raw RGB representation, as an array of bytes.
 * As a side effect the function populates widthptr, heigthptr with the
 * size of the image in pixel. The integer pointed by alphaptr is set to one.
 * if the image is of type RGB_ALPHA, otherwise it's set to zero.
 *
 * This function is able to load both RGB and RGBA images, but it will always
 * return data as RGB, discarding the alpha channel. */
#define PNG_BYTES_TO_CHECK 8
unsigned char *PngLoad(FILE *fp, int *widthptr, int *heightptr, int *alphaptr) {
    unsigned char buf[PNG_BYTES_TO_CHECK];
    png_structp png_ptr;
    png_infop info_ptr;
    png_uint_32 width, height, j;
    int color_type, row_bytes;
    unsigned char **imageData, *rgb;

    /* Check signature */
    if (fread(buf, 1, PNG_BYTES_TO_CHECK, fp) != PNG_BYTES_TO_CHECK)
        return NULL;
    if (png_sig_cmp(buf, (png_size_t)0, PNG_BYTES_TO_CHECK))
        return NULL; /* Not a PNG image */

    /* Initialize data structures */
    png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING,
        NULL,NULL,NULL);
    if (png_ptr == NULL) {
        return NULL; /* Out of memory */
    }

    info_ptr = png_create_info_struct(png_ptr);
    if (info_ptr == NULL) {
        png_destroy_read_struct(&png_ptr, NULL, NULL);
        return NULL;
    }

    /* Error handling code */
    if (setjmp(png_jmpbuf(png_ptr)))
    {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return NULL;
    }

    /* Set the I/O method */
    png_init_io(png_ptr, fp);

    /* Undo the fact that we read some data to detect the PNG file */
    png_set_sig_bytes(png_ptr, PNG_BYTES_TO_CHECK);

    /* Read the PNG in memory at once */
    png_read_png(png_ptr, info_ptr, PNG_TRANSFORM_IDENTITY, NULL);

    /* Get image info */
    width = png_get_image_width(png_ptr, info_ptr);
    height = png_get_image_height(png_ptr, info_ptr);
    color_type = png_get_color_type(png_ptr, info_ptr);
    if (color_type != PNG_COLOR_TYPE_RGB &&
        color_type != PNG_COLOR_TYPE_RGB_ALPHA) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return NULL;
    }

    /* Get the image data */
    imageData = png_get_rows(png_ptr, info_ptr);
    row_bytes = png_get_rowbytes(png_ptr, info_ptr);
    rgb = malloc(row_bytes*height);
    if (!rgb) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return NULL;
    }

    for (j = 0; j < height; j++) {
        unsigned char *dst = rgb+(j*width*3);
        unsigned char *src = imageData[j];
        unsigned int i;

        for (i = 0; i < width; i++) {
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst += 3;
            src += (color_type == PNG_COLOR_TYPE_RGB_ALPHA) ? 4 : 3;
        }
    }

    /* Free the image and resources and return */
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    *widthptr = width;
    *heightptr = height;
    *alphaptr = (color_type == PNG_COLOR_TYPE_RGB_ALPHA);
    return rgb;
}

/* Return a random number between the internal specified (including min and max) */
int randbetween(int min, int max) {
    return min+(random()%(max-min+1));
}

/* When we mutate a triangle, or create a random one, it is possible that the
 * result is invalid: coordinates out of the screen or the points not ordered
 * by 'y' (that is required for our triangle drawing algorith).
 *
 * This function normalizes it turning an invalid triangle into a valid one. */
void normalizeTriangle(struct triangle *r, int width, int height) {
    int swap, t;

    do {
        swap = 0;
        if (r->u.t.y1 > r->u.t.y2) {
            t = r->u.t.y1; r->u.t.y1 = r->u.t.y2; r->u.t.y2 = t;
            t = r->u.t.x1; r->u.t.x1 = r->u.t.x2; r->u.t.x2 = t;
            swap++;
        }
        if (r->u.t.y2 > r->u.t.y3) {
            t = r->u.t.y2; r->u.t.y2 = r->u.t.y3; r->u.t.y3 = t;
            t = r->u.t.x2; r->u.t.x2 = r->u.t.x3; r->u.t.x3 = t;
            swap++;
        }
    } while(swap);
    if (r->u.t.x1 < 0) r->u.t.x1 = 0;
    if (r->u.t.x1 >= width) r->u.t.x1 = width-1;
    if (r->u.t.y1 < 0) r->u.t.y1 = 0;
    if (r->u.t.y1 >= height) r->u.t.y1 = height-1;

    if (r->u.t.x2 < 0) r->u.t.x2 = 0;
    if (r->u.t.x2 >= width) r->u.t.x2 = width-1;
    if (r->u.t.y2 < 0) r->u.t.y2 = 0;
    if (r->u.t.y2 >= height) r->u.t.y2 = height-1;

    if (r->u.t.x3 < 0) r->u.t.x3 = 0;
    if (r->u.t.x3 >= width) r->u.t.x3 = width-1;
    if (r->u.t.y3 < 0) r->u.t.y3 = 0;
    if (r->u.t.y3 >= height) r->u.t.y3 = height-1;
}

/* When we mutate a circle, or create a random one, it is possible that the
 * result is invalid: coordinates outside the screen, or radius too big for the
 * circle to be fully contained inside the screen.
 *
 * This function normalizes it turning an invalid circle into a valid one. */
void normalizeCircle(struct triangle *r, int width, int height) {
    int x,y,radius;

    if (r->u.c.x1 < 0) r->u.c.x1 = 0;
    if (r->u.c.x1 >= width) r->u.c.x1 = width-1;
    if (r->u.c.y1 < 0) r->u.c.y1 = 0;
    if (r->u.c.y1 >= height) r->u.c.y1 = height-1;

    x = r->u.c.x1;
    y = r->u.c.y1;
    radius = r->u.c.radius;
    while(x-radius < 0 || x+radius >= width ||
          y-radius < 0 || y+radius >= height) {
          radius--;
    }
    r->u.c.radius = radius;
}

/* Calls triangle or circle normalization functions according to the type. */
void normalize(struct triangle *r, int width, int height) {
    if (r->type == TYPE_TRIANGLE)
        normalizeTriangle(r,width,height);
    else
        normalizeCircle(r,width,height);
}

/* Triangle or circle? */
int selectShapeType(void) {
    int triangle;

    if (opt_use_circles && opt_use_triangles) {
        triangle = random()&1;
    } else if (opt_use_circles) {
        triangle = 0;
    } else {
        triangle = 1;
    }
    return triangle ? TYPE_TRIANGLE : TYPE_CIRCLE;
}

/* Set random RGB and alpha. */
void setRandomColor(struct triangle *r) {
    r->r = random()%256;
    r->g = random()%256;
    r->b = random()%256;
    r->alpha = randbetween(MINALPHA,MAXALPHA);
}

/* Set random triangle vertexes or circle center and radius. */
void setRandomVertexes(struct triangle *r, int width, int height) {
    if (r->type == TYPE_TRIANGLE) {
        r->u.t.x1 = random()%width;
        r->u.t.y1 = random()%height;
        r->u.t.x2 = random()%width;
        r->u.t.y2 = random()%height;
        r->u.t.x3 = random()%width;
        r->u.t.y3 = random()%height;
    } else {
        r->u.c.x1 = random()%width;
        r->u.c.y1 = random()%height;
        r->u.c.radius = random()%width;
    }
}

/* Translate vertexes at random, from -delta to delta. */
void moveVertexes(struct triangle *t, int delta) {
    if (t->type == TYPE_TRIANGLE) {
        t->u.t.x1 += randbetween(-delta,delta);
        t->u.t.y1 += randbetween(-delta,delta);
        t->u.t.x2 += randbetween(-delta,delta);
        t->u.t.y2 += randbetween(-delta,delta);
        t->u.t.x3 += randbetween(-delta,delta);
        t->u.t.y3 += randbetween(-delta,delta);
    } else {
        t->u.c.x1 += randbetween(-delta,delta);
        t->u.c.y1 += randbetween(-delta,delta);
        t->u.c.radius += randbetween(-delta,delta);
    }
}

/* Create a random triangle or circle. */
void randomtriangle(struct triangle *r, int width, int height) {
    r->type = selectShapeType();
    setRandomVertexes(r,width,height);
    setRandomColor(r);
    normalize(r,width,height);
}

/* Like randomtriangle() but vertex/radius can't be more than 'delta' pixel
 * away from initial random coordinates. */
void randomsmalltriangle(struct triangle *r, int width, int height, int delta) {
    int x = random()%width;
    int y = random()%height;

    r->type = selectShapeType();
    if (r->type == TYPE_TRIANGLE) {
        r->u.t.x1 = x + randbetween(-delta,delta);
        r->u.t.y1 = y + randbetween(-delta,delta);
        r->u.t.x2 = x + randbetween(-delta,delta);
        r->u.t.y2 = y + randbetween(-delta,delta);
        r->u.t.x3 = x + randbetween(-delta,delta);
        r->u.t.y3 = y + randbetween(-delta,delta);
    } else {
        r->u.c.x1 = x;
        r->u.c.y1 = y;
        r->u.c.radius = randbetween(1,delta);
    }
    setRandomColor(r);
    normalize(r,width,height);
}

/* Apply a random mutation to the specified triangle/circle. */
void mutatetriangle(struct triangle *t, int width, int height) {
    int choice = random() % 6;

    if (choice == 0) {
        setRandomVertexes(t,width,height);
        normalize(t,width,height);
    } else if (choice == 1) {
        moveVertexes(t,20);
        normalize(t,width,height);
    } else if (choice == 2) {
        moveVertexes(t,5);
        normalize(t,width,height);
    } else if (choice == 3) {
        t->r = random()%256;
        t->g = random()%256;
        t->b = random()%256;
    } else if (choice == 4) {
        int r,g,b;

        r = t->r + randbetween(-5,5);
        g = t->g + randbetween(-5,5);
        b = t->b + randbetween(-5,5);
        if (r < 0) r = 0;
        else if (r > 255) r = 255;
        if (g < 0) g = 0;
        else if (g > 255) g = 255;
        if (b < 0) b = 0;
        else if (b > 255) b = 255;
        t->r = r;
        t->g = g;
        t->b = b;
    } else if (choice == 5) {
        t->alpha = randbetween(MINALPHA,MAXALPHA);
    }
}

/* Create a set of trinalges and populate it with random triangles. */
struct triangles *mkRandomtriangles(int count, int width, int height) {
    struct triangles *rs;
    int j;

    rs = malloc(sizeof(*rs));
    rs->count = count;
    rs->inuse = 1;
    rs->triangles = malloc(sizeof(struct triangle)*count);
    for (j = 0; j < count; j++) {
        struct triangle *r = &rs->triangles[j];
        randomtriangle(r,width,height);
    }
    return rs;
}

/* Draw a prixel in an RGB framebuffer. */
void setPixelWithAlpha(unsigned char *fb, int x, int y, int width, int height, int r, int g, int b, float alpha) {
    unsigned char *p = fb+y*width*3+x*3;

    if (x < 0 || x >= width || y < 0 || y >= height) return;
    p[0] = (alpha*r)+((1-alpha)*p[0]);
    p[1] = (alpha*g)+((1-alpha)*p[1]);
    p[2] = (alpha*b)+((1-alpha)*p[2]);
}

/* Draw an horizontal line in an RGB framebuffer. */
void drawHline(unsigned char *fb, int width, int height, int x1, int x2, int y, int r, int g, int b, float alpha) {
    int aux, x;
    unsigned char *p;
    int ar = alpha*r;
    int ag = alpha*g;
    int ab = alpha*b;
    float invalpha = 1-alpha;

    if (y < 0 || y >= height) return;
    if (x1 > x2) {
        aux = x1;
        x1 = x2;
        x2 = aux;
    }
    p = fb+y*width*3+x1*3;
    for (x = x1; x <= x2; x++) {
        p[0] = ar+(invalpha*p[0]);
        p[1] = ag+(invalpha*p[1]);
        p[2] = ab+(invalpha*p[2]);
        p += 3;
    }
}

/* Draw a circle in an RGB framebuffer. */
void drawCircle(unsigned char *fb, int width, int height, struct triangle *c)
{
    int x1, x2, y;
    int xc, yc, r;

    xc = c->u.c.x1;
    yc = c->u.c.y1;
    r = c->u.c.radius;

    for (y=yc-r; y<=yc+r; y++) {
        x1 = round(xc + sqrt((r*r) - ((y - yc)*(y - yc))));
        x2 = round(xc - sqrt((r*r) - ((y - yc)*(y - yc))));
        drawHline(fb,width,height,x1,x2,y,c->r,c->g,c->b,(float)c->alpha/100);
    }
}

/* Draw a triangle in an RGB framebuffer. */
void drawTriangle(unsigned char *fb, int width, int height, struct triangle *r) {
    struct {
        float x, y;
    } A, B, C, E, S;
    float dx1,dx2,dx3;

    A.x = r->u.t.x1;
    A.y = r->u.t.y1;
    B.x = r->u.t.x2;
    B.y = r->u.t.y2;
    C.x = r->u.t.x3;
    C.y = r->u.t.y3;

    if (B.y-A.y > 0) dx1=(B.x-A.x)/(B.y-A.y); else dx1=B.x - A.x;
    if (C.y-A.y > 0) dx2=(C.x-A.x)/(C.y-A.y); else dx2=0;
    if (C.y-B.y > 0) dx3=(C.x-B.x)/(C.y-B.y); else dx3=0;

    S=E=A;
    if(dx1 > dx2) {
        for(;S.y<=B.y;S.y++,E.y++,S.x+=dx2,E.x+=dx1)
            drawHline(fb,width,height,S.x,E.x,S.y,r->r,r->g,r->b,(float)r->alpha/100);
        E=B;
        E.y+=1;
        for(;S.y<=C.y;S.y++,E.y++,S.x+=dx2,E.x+=dx3)
            drawHline(fb,width,height,S.x,E.x,S.y,r->r,r->g,r->b,(float)r->alpha/100);
    } else {
        for(;S.y<=B.y;S.y++,E.y++,S.x+=dx1,E.x+=dx2)
            drawHline(fb,width,height,S.x,E.x,S.y,r->r,r->g,r->b,(float)r->alpha/100);
        S=B;
        S.y+=1;
        for(;S.y<=C.y;S.y++,E.y++,S.x+=dx3,E.x+=dx2)
            drawHline(fb,width,height,S.x,E.x,S.y,r->r,r->g,r->b,(float)r->alpha/100);
    }
}

/* Draw a set of trinalges/circles in an RGB framebuffer. */
void drawtriangles(unsigned char *fb, int width, int height, struct triangles *r) {
    int j;

    for (j = 0; j < r->inuse; j++) {
        if (r->triangles[j].type == TYPE_TRIANGLE)
            drawTriangle(fb,width,height,&r->triangles[j]);
        else
            drawCircle(fb,width,height,&r->triangles[j]);
    }
}

/* Compute the difference between two RGB frame buffers.
 * The differece is the sum of the differences of every pixel at the same
 * coordinates in the two images.
 *
 * A single pixel difference is computed as spacial distance between the RGB
 * color space. */
long long computeDiff(unsigned char *a, unsigned char *b, int width, int height) {
    int j;
    long long d = 0;
    long long dr, dg, db;

    for (j = 0; j < width*height*3; j+=3) {
        dr = (int)a[j]-(int)b[j];
        dg = (int)a[j+1]-(int)b[j+1];
        db = (int)a[j+2]-(int)b[j+2];
        d += sqrt(dr*dr+dg*dg+db*db);
    }
    return d;
}

/* Apply a mutation to a set of triangles. */
void mutatetriangles(struct triangles *rs, int count, int width, int height) {
    int j;

    /* Add a new triangle? */
    if ((random() % 10) == 0) {
        if (rs->inuse != rs->count &&
            rs->inuse < state.max_shapes_incremental)
        {
            int r = random() % 5;
            if (r == 0) {
                randomtriangle(&rs->triangles[rs->inuse],width,height);
            } else if (r == 1) {
                randomsmalltriangle(&rs->triangles[rs->inuse],width,height,5);
            } else if (r == 2) {
                randomsmalltriangle(&rs->triangles[rs->inuse],width,height,10);
            } else if (r == 3) {
                randomsmalltriangle(&rs->triangles[rs->inuse],width,height,25);
            } else if (r == 4) {
                randomsmalltriangle(&rs->triangles[rs->inuse],width,height,2);
            }
            rs->inuse++;
            return;
        }
    }

    /* Remove a triangle? */
    if ((random() % 20) == 0) {
        if (rs->inuse > 1) {
            int delidx = random() % rs->inuse;

            rs->inuse--;
            memmove(rs->triangles+delidx,rs->triangles+delidx+1,sizeof(struct triangle)*(rs->inuse-delidx));
            return;
        }
    }

    /* Swap two triangles */
    if ((random() % 20) == 0) {
        int a, b;
        a = random()%rs->inuse;
        b = random()%rs->inuse;
        if (a != b) {
            struct triangle aux;

            aux = rs->triangles[a];
            rs->triangles[a] = rs->triangles[b];
            rs->triangles[b] = aux;
        }
    }

    /* Mutate every single triangle. */
    for (j = 0; j < count; j++) {
        struct triangle *r = &rs->triangles[random()%rs->inuse];
        if (random() % 1000 < opt_mutation_rate) mutatetriangle(r,width,height);
    }
}

/* Save a set of triangles as SVG. */
void saveSvg(char *filename,struct triangles *triangles) {
    FILE *fp = fopen(filename,"w");
    int j;

    fprintf(fp,"<?xml version=\"1.0\" standalone=\"no\"?><!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.1//EN\" \"http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd\"><svg width=\"100%%\" height=\"100%%\" style=\"background-color:#000000;\" version=\"1.1\" xmlns=\"http://www.w3.org/2000/svg\">\n");
    for(j=0;j<triangles->inuse;j++) {
        struct triangle *t = &triangles->triangles[j];
        if (t->type == TYPE_TRIANGLE) {
            fprintf(fp,"<polygon points=\"%d,%d %d,%d %d,%d\" style=\"fill:#%02x%02x%02x;stroke:#000000;stroke-width:0;fill-opacity:%.2f;\"/>\n",t->u.t.x1,t->u.t.y1,t->u.t.x2,t->u.t.y2,t->u.t.x3,t->u.t.y3,t->r,t->g,t->b,(float)t->alpha/100);
        } else if (t->type == TYPE_CIRCLE) {
            fprintf(fp,"<circle cx=\"%d\" cy=\"%d\" r=\"%d\" style=\"fill:#%02x%02x%02x;stroke:#000000;stroke-width:0;fill-opacity:%.2f;\"/>\n",t->u.c.x1,t->u.c.y1,t->u.c.radius,t->r,t->g,t->b,(float)t->alpha/100);
        }
    }
    fprintf(fp,"</svg>\n");
    fclose(fp);
}

/* Save a binary representation of a set of triangles and the program state. */
void saveBinary(char *filename,struct triangles *triangles) {
    FILE *fp = fopen(filename,"wb");

    if (!fp) {
        perror("opening binary file");
        exit(1);
    }
    fwrite(&state,sizeof(state),1,fp);
    fwrite(triangles,sizeof(*triangles),1,fp);
    fwrite(triangles->triangles,sizeof(struct triangle)*triangles->inuse,1,fp);
    fclose(fp);
}

/* Load a binary representation of a set of triangles and the program state. */
void loadBinary(char *filename,struct triangles *triangles) {
    FILE *fp = fopen(filename,"rb");
    struct triangle *tarray;

    if (!fp) return; /* If there is no file we start with a clear state. */

    /* Load the state structure, and the best solution. */
    if (fread(&state,sizeof(state),1,fp) != 1) goto loaderr;
    free(triangles->triangles);
    if (fread(triangles,sizeof(*triangles),1,fp) != 1) goto loaderr;
    if (triangles->inuse > state.max_shapes) {
        fprintf(stderr,
            "Can't load a binary image with more than %d triangles\n",
            state.max_shapes);
        exit(1);
    }
    printf("Loading %d triangles\n", triangles->inuse);
    tarray = malloc(sizeof(struct triangle)*state.max_shapes);
    if (fread(tarray,sizeof(struct triangle)*triangles->inuse,1,fp) != 1)
        goto loaderr;
    triangles->triangles = tarray;
    triangles->count = state.max_shapes;

    /* Let the program continue with the current number of triangles. */
    state.max_shapes_incremental = triangles->inuse;
    fclose(fp);
    printf("Loaded\n");
    return;

loaderr:
    fprintf(stderr, "Error loading the binary file");
    exit(1);
}

void showHelp(char *progname) {
    fprintf(stderr,
        "Usage: %s <filename.png> <filename.bin> <filename.svg> [options]\n"
        "\n"
        "--use-triangles   <0 or 1> default: 1.\n"
        "--use-circles     <0 or 1> default: 0.\n"
        "--max-shapes      <count> default: 64.\n"
        "--initial-shapes  <count> default: 1.\n"
        "--mutation-rate   <count> From 0 to 1000, default: 200\n"
        "--restart         Don't load the old state at startup.\n"
        "--help            Just show this help.\n"
        ,progname);
    exit(1);
}

int main(int argc, char **argv)
{
    FILE *fp;
    int width, height, alpha;
    unsigned char *image, *fb;
    SDL_Surface *screen;
    struct triangles *triangles, *best, *absbest;
    long long diff;
    float percdiff, bestdiff;

    /* Initialization */
    srand(time(NULL));
    state.max_shapes = 64;
    state.max_shapes_incremental = 1;
    state.temperature = 0.10;
    state.generation = 0;
    state.absbestdiff = 100; /* 100% is worst diff possible. */

    /* Check arity and parse additional args if any. */
    if (argc < 4) {
        showHelp(argv[0]);
        exit(1);
    }

    if (argc > 4) {
        int j;
        for (j = 4; j < argc; j++) {
            int moreargs = j+1 < argc;

            if (!strcmp(argv[j],"--use-triangles") && moreargs) {
                opt_use_triangles = atoi(argv[++j]);
            } else if (!strcmp(argv[j],"--use-circles") && moreargs) {
                opt_use_circles = atoi(argv[++j]);
            } else if (!strcmp(argv[j],"--max-shapes") && moreargs) {
                state.max_shapes = atoi(argv[++j]);
            } else if (!strcmp(argv[j],"--initial-shapes") && moreargs) {
                state.max_shapes_incremental = atoi(argv[++j]);
            } else if (!strcmp(argv[j],"--mutation-rate") && moreargs) {
                opt_mutation_rate = atoi(argv[++j]);
            } else if (!strcmp(argv[j],"--restart")) {
                opt_restart = 1;
            } else if (!strcmp(argv[j],"--help")) {
                showHelp(argv[0]);
            } else {
                fprintf(stderr,"Invalid options.");
                showHelp(argv[0]);
                exit(1);
            }
        }
    }

    /* Sanity check. */
    if (state.max_shapes_incremental > state.max_shapes)
        state.max_shapes = state.max_shapes_incremental;
    if (opt_mutation_rate > 1000)
        opt_mutation_rate = 1000;

    /* Load the PNG in memory. */
    fp = fopen(argv[1],"rb");
    if (!fp) {
        perror("Opening PNG file");
        exit(1);
    }
    if ((image = PngLoad(fp,&width,&height,&alpha)) == NULL) {
        printf("Can't load the specified image.");
        exit(1);
    }

    printf("Image %d %d, alpha:%d at %p\n", width, height, alpha, image);
    fclose(fp);

    /* Initialize SDL and allocate our arrays of triangles. */
    screen = sdlInit(width,height,0);
    fb = malloc(width*height*3);
    triangles = mkRandomtriangles(state.max_shapes,width,height);
    best = mkRandomtriangles(state.max_shapes,width,height);
    absbest = mkRandomtriangles(state.max_shapes,width,height);
    state.absbestdiff = bestdiff = 100;

    /* Load the binary file if any. */
    if (!opt_restart) {
        loadBinary(argv[2],best);
    } else {
        best->inuse = state.max_shapes_incremental;
    }
    absbest->inuse = best->inuse;
    memcpy(absbest->triangles,best->triangles,
        sizeof(struct triangle)*best->count);

    /* Show the current evolved image and the real image for one scond each. */
    memset(fb,0,width*height*3);
    drawtriangles(fb,width,height,best);
    sdlShowRgb(screen,fb,width,height);
    sleep(1);
    sdlShowRgb(screen,image,width,height);
    sleep(1);

    /* Evolve the current solution using simulated annealing. */
    while(1) {
        state.generation++;
        if (state.temperature > 0 && !(state.generation % 10)) {
            state.temperature -= 0.00001;
            if (state.temperature < 0) state.temperature = 0;
        }

        /* From time to time allow the current solution to use one more
         * triangle, up to the configured max number. */
        if ((state.generation % 1000) == 0) {
            if (state.max_shapes_incremental < triangles->count &&
                triangles->inuse > state.max_shapes_incremental-1)
            {
                state.max_shapes_incremental++;
            }
        }

        /* Copy what is currenly the best solution, and mutate it. */
        memcpy(triangles->triangles,best->triangles,
            sizeof(struct triangle)*best->count);
        triangles->inuse = best->inuse;
        mutatetriangles(triangles,10,width,height);

        /* Draw the mutated solution, and check what is its fitness.
         * In our case the fitness is the difference bewteen the target
         * image and our image. */
        memset(fb,0,width*height*3);
        drawtriangles(fb,width,height,triangles);
        diff = computeDiff(image,fb,width,height);

        /* The percentage of difference is calculate taking the ratio between
         * the maximum difference and the current difference.
         * The magic constant 422 is actually the max difference between
         * two pixels as r,g,b coordinates in the space, so sqrt(255^2*3). */
        percdiff = (float)diff/(width*height*442)*100;
        if (percdiff < bestdiff ||
            (state.temperature > 0 &&
             ((float)rand()/RAND_MAX) < state.temperature &&
             (percdiff-state.absbestdiff) < 2*state.temperature))
        {
            /* Save what is currently our "best" solution, even if actually
             * this may be a jump backward depending on the temperature.
             * It will be used as a base of the next iteration. */
            best->inuse = triangles->inuse;
            memcpy(best->triangles,triangles->triangles,
                sizeof(struct triangle)*best->count);

            if (percdiff < bestdiff) {
                /* We always save a copy of the absolute best solution we found
                 * so far, after some generation without finding anything better
                 * we may jump back to that solution.
                 *
                 * We also use the absolute best solution to save the program
                 * state in the binary file, and as SVG output. */
                absbest->inuse = best->inuse;
                memcpy(absbest->triangles,best->triangles,
                    sizeof(struct triangle)*best->count);
                state.absbestdiff = percdiff;
            }

            printf("Diff is %f%% (inuse:%d, max:%d, gen:%lld, temp:%f)\n",
                percdiff,
                triangles->inuse,
                state.max_shapes_incremental,
                state.generation,
                state.temperature);

            bestdiff = percdiff;
            sdlShowRgb(screen,fb,width,height);
        }
        processSdlEvents();

        /* From time to time save the current state into a binary save
         * and produce an SVG of the current solution. */
        if ((state.generation % 100) == 0) {
            saveSvg(argv[3],absbest);
            saveBinary(argv[2],absbest);
        }
    }
    return 0;
}
