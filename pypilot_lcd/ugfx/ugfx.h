/* Copyright (C) 2016 Sean D'Epagnier <seandepagnier@gmail.com>
 *
 * This Program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 */

#include <linux/fb.h>

unsigned int color(int r, int g, int b);

class surface
{
public:
    surface(surface *s);
    surface(int w, int h, int internal_bypp, const char *data32);
    surface(const char* filename);
    virtual ~surface();

    void store_grey(const char *filename);
    void blit(surface *src, int xoff, int yoff);
    void magnify(int factor);
    void putpixel(int x, int y, unsigned int c);
    void line(int x1, int y1, int x2, int y2, unsigned int c);
    void hline(int x1, int x2, int y, unsigned int c);
    void vline(int x, int y1, int y2, unsigned int c);
    void box(int x1, int y1, int x2, int y2, unsigned int c);
    void invert(int x1, int y1, int x2, int y2);
    void fill(unsigned int c);

    int width, height, bypp;
    char *p;
    int getpixel(int x, int y);
    unsigned char *ptr() { return (unsigned char*)p; }

    int xoffset, yoffset, line_length;

protected:
    surface() {}
};


class screen : public surface
{
public:
    screen(const char *device);
    virtual ~screen();

    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;

    char *fbp;
    int fbfd;
    long int screensize;
};

