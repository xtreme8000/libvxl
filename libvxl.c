#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "libvxl.h"

//*** INTERNAL FUNCTIONS START ***

static int libvxl_span_length(struct libvxl_span* s) {
    return s->length>0?s->length*4:(s->color_end-s->color_start+2)*4;
}

static void libvxl_column_push(struct libvxl_column* c, struct libvxl_span* s) {
    int l = libvxl_span_length(s);
    if(c->index+l-1>=c->length) {
        c->length += l+8;
        c->spans = realloc(c->spans,c->length);
    }
    memcpy(c->spans+c->index,s,l);
    c->index += l;
}

static struct libvxl_column* libvxl_getcolumn(struct libvxl_map* map, int x, int y) {
    return &map->columns[x+map->width*y];
}

//*** INTERNAL FUNCTIONS END ***



void libvxl_create(struct libvxl_map* map, int w, int h, int d, void* data) {
    map->width = w;
    map->height = h;
	map->depth = d;
    map->columns = calloc(sizeof(struct libvxl_column),map->width*map->height);

    int offset = 0;
    int x = 0;
    int y = 0;
    while(1) {
        struct libvxl_span* desc = (struct libvxl_span*)(data+offset);
        libvxl_column_push(libvxl_getcolumn(map,x,y),desc);

        offset += libvxl_span_length(desc);

        if(desc->length==0 && (++x)==map->width) {
            x = 0;
            if((++y)==map->height)
                break;
        }

    }
}

void libvxl_free(struct libvxl_map* map) {
    for(int k=0;k<map->width*map->height;k++)
        free(map->columns[k].spans);
    free(map->columns);
}

int libvxl_map_issolid(struct libvxl_map* map, int x, int y, int z) {
	if(x<0 || y<0 || z<0 || x>=map->width || y>=map->height || z>=map->depth)
		return 0;
	struct libvxl_column* c = libvxl_getcolumn(map,x,y);
	int offset = 0;
	while(1) {
		struct libvxl_span* desc = (struct libvxl_span*)(c->spans+offset);
		//printf("n: %i a: %i s: %i e: %i\n",desc->length,desc->air_start,desc->color_start,desc->color_end);
		if(z>=desc->air_start && z<desc->color_start)
			return 0;
		//if(z<desc->air_start)
			//return 1;
		if(desc->length==0)
			break;
		offset += libvxl_span_length(desc);
	}
	return 1;
}

int libvxl_map_get(struct libvxl_map* map, int x, int y, int z) {
	if(x<0 || y<0 || z<0 || x>=map->width || y>=map->height || z>=map->depth)
		return 0;
	struct libvxl_column* c = libvxl_getcolumn(map,x,y);
	int offset = 0;
	while(1) {
		struct libvxl_span* desc = (struct libvxl_span*)(c->spans+offset);
		int* color_data = (int*)(c->spans+offset+sizeof(struct libvxl_span));
		struct libvxl_span* desc_next = (struct libvxl_span*)(c->spans+offset+libvxl_span_length(desc));


		if(z>=desc->air_start && z<desc->color_start) //check for air
			return 0;
		if(z>=desc->color_start && z<=desc->color_end) //check for top colors
			return color_data[z-desc->color_start]&0xFFFFFF;

		int top_len = desc->color_end-desc->color_start+1;
		int bottom_len = desc->length-1-top_len;

		if(desc->length>0 && z>=desc_next->air_start-bottom_len && z<desc_next->air_start) //check for bottom colors
			return color_data[z-(desc_next->air_start-bottom_len)+top_len]&0xFFFFFF;

		//if(desc->air_start>z)
			//return 0;
		if(desc->length==0)
			break;
		offset += libvxl_span_length(desc);
	}
	return 0; //dirt color
}

int libvxl_map_gettop(struct libvxl_map* map, int x, int y) {
	if(x<0 || y<0 || x>=map->width || y>=map->height)
		return 0;
	struct libvxl_column* c = libvxl_getcolumn(map,x,y);
	return *(int*)(c->spans+sizeof(struct libvxl_span))&0xFFFFFF;
}
