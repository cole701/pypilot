/* Copyright (C) 2016 Sean D'Epagnier <seandepagnier@gmail.com>
 *
 * This Program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 */

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include "ugfx.h"

static uint16_t color16(uint32_t c)
{
    return (c&0xfc)<<11 | (c&0xfd00) >> 3 | (c&0xfc0000) >> 16;
}

static uint16_t color16gray(uint8_t c)
{
    return (c&0xfc)<<11 | (c&0xfc)<<5 | (c&0xfc);
}

uint32_t color(int r, int g, int b)
{
    return (r<<16) + (g<<8) + (b);
}

surface::surface(surface *s)
{
    xoffset = yoffset = 0;
    width = s->width, height = s->height;
    bypp = s->bypp;
    p = new char [width*height*bypp];
    memcpy(p, s->p, width*height*bypp);
    line_length = width*bypp;
}

surface::surface(int w, int h, int internal_bypp, const char *data32)
{
    xoffset = yoffset = 0;
    width = w, height = h;
    bypp = internal_bypp;
    p = new char [width*height*bypp];
    line_length = width*bypp;

    if (!data32)
        return;

    if (bypp == 2) {
        int i = 0;
        for(int y = 0; y<height; y++)
            for(int x = 0; x<width; x++){
                uint32_t c = *(uint32_t*)(data32 + i);
                uint16_t t = (c&0xfc)<<11 | (c&0xfd00) >> 3 | (c&0xfc0000) >> 16;
                long location = x * bypp + y * line_length;
                *((uint16_t*)(p + location)) = t;
                i++;
            }
    } else
        memcpy(p, data32, 4*width*height);
}


surface::surface(const char* filename)
{
    width = height = bypp = 0;
    p = NULL;

    FILE *f = fopen(filename, "r");
    if(!f)
        return;

    uint16_t file_bypp, colors;
    if(fread(&width, 2, 1, f) != 1 || fread(&height, 2, 1, f) != 1 ||
       fread(&file_bypp, 2, 1, f) != 1 || fread(&colors, 2, 1, f) != 1)
        goto fail;

    xoffset = yoffset = 0;
    p = new char [width*height*file_bypp];
    line_length = width*file_bypp;

    if(colors != 1) // only greyscale supported
        goto fail;  

    {
        char gray_data[width*height];
        unsigned int i=0;
        while(i<sizeof gray_data) {
            uint8_t run, value;
            if(fread(&run, 1, 1, f) != 1 || fread(&value, 1, 1, f) != 1) {
                printf("fail\n");
                goto fail;
            }
            while(run-- > 0)
            gray_data[i++] = value;
        }

        if(file_bypp == 2)
            for(int i = 0; i<width*height; i++)
                ((uint16_t*)p)[i] = color16gray(gray_data[i]);
        else
            for(int i = 0; i<width*height; i++)
                memset(p + 4*i, gray_data[i], 3);
    }

    bypp = file_bypp;

fail:
    fclose(f);
    return;
}

surface::~surface()
{
    delete [] p;
}

void surface::store_grey(const char *filename)
{
    char gray_data[width*height];
    for(unsigned int i=0; i<sizeof gray_data; i++)
        if(bypp == 2)
            gray_data[i] = p[2*i]&0xfc;
        else
            gray_data[i] = p[4*i];

    FILE *f = fopen(filename, "w");
    uint16_t colors = 1; // grey
    fwrite(&width, 1, 2, f);
    fwrite(&height, 1, 2, f);
    fwrite(&bypp, 1, 2, f);
    fwrite(&colors, 1, 2, f);

    char last = 0;
    uint8_t run = 0;
    for(int i=0; i<sizeof gray_data; i++) {
        if(gray_data[i] == last) {
            if(run == 255) {
                fwrite(&run, 1, 1, f);
                fwrite(&last, 1, 1, f);
                run = 0;
            }
            run++;
        } else {
            if(run > 0) {
                fwrite(&run, 1, 1, f);
                fwrite(&last, 1, 1, f);
            }
            last = gray_data[i];
            run = 1;
        }
    }
    
    fwrite(&run, 1, 1, f);
    fwrite(&last, 1, 1, f);
    fclose(f);
}

void surface::blit(surface *src, int xoff, int yoff)
{
    if(bypp != src->bypp) {
        printf("incompatible surfaces cannot be blit\n");
        return;
    }

    int w = src->width, h = src->height, xsoff = 0, ysoff = 0;

    long src_location = 0;
    if (xoff < 0) {
        src_location -= bypp*xoff;
        w += xoff;
        xoff = 0;
    }

    if (yoff < 0) {
        src_location -= src->line_length*yoff;
        h += yoff;
        yoff = 0;
    }
    
    if (xoff + w > width)
        w = width - xoff;
    if (yoff + h > height)
        h = height - yoff;

    for(int y = 0; y<h; y++) {
        long dest_location = (xoff+xoffset) * bypp + (y+yoff+yoffset) * line_length;
        memcpy(p + dest_location, src->p + src_location, bypp*w);
        src_location += src->line_length;
    }
}

void surface::magnify(int factor)
{
    char *q = new char [factor*factor*width*height*bypp];

    long sl = 0;
    for(int y = 0; y<height; y++)
        for(int x = 0; x<width; x++) {
            long dl = factor * (x*bypp + y*factor*line_length);
            for(int i = 0; i < factor; i++) {
                for(int j = 0; j < factor; j++) {
                    memcpy(q + dl, p + sl, bypp);
                    dl += bypp;
                }
                dl += factor*(line_length - bypp);
            }
            sl+=bypp;
        }

    delete [] p;

    width *= factor, height *= factor, line_length *= factor;
    p = q;
}

void surface::putpixel(int x, int y, uint32_t c)
{
    long dl = x * bypp + y * line_length;
    if(bypp == 2) {
        uint16_t t = color16(t);
        *(uint16_t*)(p + dl) = c;
    } else
        *(uint32_t*)(p + dl) = c;
}

void surface::line(int x1, int y1, int x2, int y2, uint32_t c)
{
    if (abs(x2 - x1) > abs(y2 - y1)) {
        if (x2 < x1)
            line(x2, y2, x1, y1, c);
        else
            for(int x = x1; x < x2; x++) {
                int y = (y2 - y1)*(x - x1)/(x2 - x1) + y1;
                putpixel(x, y, c);
            }
    } else {
        if (y2 < y1)
            line(x2, y2, x1, y1, c);
        else
            for(int y = y1; y < y2; y++) {
                int x = (x2 - x1)*(y - y1)/(y2 - y1) + x1;
                putpixel(x, y, c);
            }
    }
}

void surface::hline(int x1, int x2, int y, uint32_t c)
{
    if(bypp == 2) {
        uint16_t t = color16(c);
        for(int x = x1; x <= x2; x++)
            *(uint16_t*)(p + y*line_length + x*bypp) = t;
    } else
        for(int x = x1; x <= x2; x++)
            *(uint32_t*)(p + y*line_length + x*bypp) = c;
}

void surface::vline(int x, int y1, int y2, uint32_t c)
{
    if(bypp == 2) {
        uint16_t t = color16(t);
        for(int y = y1; y <= y2; y++)
            *(uint16_t*)(p + y*line_length + x*bypp) = t;
    } else
        for(int y = y1; y <= y2; y++)
            *(uint32_t*)(p + y*line_length + x*bypp) = c;
}

void surface::box(int x1, int y1, int x2, int y2, uint32_t c)
{
    if(bypp == 2) {
        uint16_t t = color16(t);
        for(int y = y1; y <= y2; y++)
            for(int x = x1; x <= x2; x++)
                *(uint16_t*)(p + y*line_length + x*bypp) = t;
    } else
        for(int y = y1; y <= y2; y++)
            for(int x = x1; x <= x2; x++)
                *(uint32_t*)(p + y*line_length + x*bypp) = c;
}

void surface::invert(int x1, int y1, int x2, int y2)
{
    x1 = x1 > 0 ? x1 : 0;
    x2 = x2 < width ? x2 : width-1;
    y1 = y1 > 0 ? y1 : 0;
    y2 = y2 < height ? y2 : height-1;
                        
    for(int y = y1; y <= y2; y++)
        for(int x = x1; x <= x2; x++)
            for(int c = 0; c<bypp; c++) {
                int i = y*line_length + x*bypp + c;
                p[i] = ~p[i];
            }
}

void surface::fill(uint32_t c)
{
    box(0, 0, width-1, height-1, c);
}

int surface::getpixel(int x, int y)
{
    if(bypp != 4) {
        fprintf(stderr, "bypp incompatible with getpixel\n");
        exit(1);
    }
    return *(uint32_t*)(p + y*line_length + x*bypp);
}


screen::screen(const char *device)
{
    int x = 0, y = 0;
    
    // Open the file for reading and writing
    fbfd = open(device, O_RDWR);
    if (fbfd == -1) {
        perror("Error: cannot open framebuffer device");
        exit(1);
    }
    printf("The framebuffer device was opened successfully.\n");

    // Get fixed screen information
    if (ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo) == -1) {
        perror("Error reading fixed information");
        exit(2);
    }

    // Get variable screen information
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) == -1) {
        perror("Error reading variable information");
        exit(3);
    }

    printf("%dx%d, %dbpp\n", vinfo.xres, vinfo.yres, vinfo.bits_per_pixel);

    // Figure out the size of the screen in bytes
    screensize = vinfo.xres * vinfo.yres * vinfo.bits_per_pixel/8;

    // Map the device to memory
    fbp = (char*)mmap(0, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fbp == (void*)-1) {
        perror("Error: failed to map framebuffer device to memory");
        exit(4);
    }
    printf("The framebuffer device was mapped to memory successfully.\n");

    p = fbp;
    width = vinfo.xres;
    height = vinfo.yres;
    bypp = vinfo.bits_per_pixel/8;
    xoffset = vinfo.xoffset;
    yoffset = vinfo.yoffset;
    line_length = finfo.line_length;
}

screen::~screen()
{
    munmap(fbp, screensize);
    p = 0;
    close(fbfd);
}