#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "libvxl.h"

static int libvxl_geometry_get(struct libvxl_map* map, int x, int y, int z) {
	int offset = x+(y+z*map->height)*map->width;
	return map->geometry[offset/8]&(1<<(offset%8));
}

static void libvxl_geometry_set(struct libvxl_map* map, int x, int y, int z, int bit) {
	if(x<0 || y<0 || z<0 || x>=map->width || y>=map->height || z>=map->depth)
		return;
	int offset = x+(y+z*map->height)*map->width;
	if(bit)
		map->geometry[offset/8] |= (1<<(offset%8));
	else
		map->geometry[offset/8] &= ~(1<<(offset%8));
}

static int cmp(const void* a, const void* b) {
	struct libvxl_block* aa = (struct libvxl_block*)a;
	struct libvxl_block* bb = (struct libvxl_block*)b;
	return aa->position-bb->position;
}

static void libvxl_chunk_put(struct libvxl_chunk* chunk, int pos, int color) {
	if(chunk->index==chunk->length) { //needs to grow
		chunk->length += CHUNK_GROWTH;
		chunk->blocks = realloc(chunk->blocks,chunk->length*sizeof(struct libvxl_block));
	}
	struct libvxl_block blk;
	blk.position = pos;
	blk.color = color;
	memcpy(&chunk->blocks[chunk->index++],&blk,sizeof(struct libvxl_block));
}

static void libvxl_chunk_insert(struct libvxl_chunk* chunk, int pos, int color) {
	if(chunk->index==0) {
		libvxl_chunk_put(chunk,pos,color);
		return;
	}
	int start = 0;
	int end = chunk->index;
	while(end-start>0) {
		int diff = pos-chunk->blocks[(start+end)/2].position;
		if(diff>0) {
			start = (start+end+1)/2;
		} else if(diff<0) {
			end = (start+end)/2;
		} else { //diff=0, replace color
			chunk->blocks[(start+end)/2].color = color;
			return;
		}
	}

	if(start>=chunk->index)
		start = chunk->index-1;

	if(chunk->index==chunk->length) { //needs to grow
		chunk->length += CHUNK_GROWTH;
		chunk->blocks = realloc(chunk->blocks,chunk->length*sizeof(struct libvxl_block));
	}

	if(pos-chunk->blocks[start].position>0) { //insert to the right of start
		memmove(chunk->blocks+start+2,chunk->blocks+start+1,(chunk->index-start-1)*sizeof(struct libvxl_block));
		chunk->blocks[start+1].position = pos;
		chunk->blocks[start+1].color = color;
	} else { //insert to the left of start
		memmove(chunk->blocks+start+1,chunk->blocks+start,(chunk->index-start)*sizeof(struct libvxl_block));
		chunk->blocks[start].position = pos;
		chunk->blocks[start].color = color;
	}
	chunk->index++;
}

static int libvxl_span_length(struct libvxl_span* s) {
	return s->length>0?s->length*4:(s->color_end-s->color_start+2)*4;
}


void libvxl_free(struct libvxl_map* map) {
	int sx = (map->width+CHUNK_SIZE-1)/CHUNK_SIZE;
	int sy = (map->height+CHUNK_SIZE-1)/CHUNK_SIZE;
	for(int k=0;k<sx*sy;k++)
		free(map->chunks[k].blocks);
	free(map->chunks);
	free(map->geometry);
}

void libvxl_create(struct libvxl_map* map, int w, int h, int d, const void* data) {
	map->width = w;
	map->height = h;
	map->depth = d;
	int sx = (w+CHUNK_SIZE-1)/CHUNK_SIZE;
	int sy = (h+CHUNK_SIZE-1)/CHUNK_SIZE;
	map->chunks = malloc(sx*sy*sizeof(struct libvxl_chunk));
	for(int y=0;y<sy;y++) {
		for(int x=0;x<sx;x++) {
			map->chunks[x+y*sx].length = CHUNK_SIZE*CHUNK_SIZE*2; //allows for two fully filled layers
			map->chunks[x+y*sx].index = 0;
			map->chunks[x+y*sx].blocks = malloc(map->chunks[x+y*sx].length*sizeof(struct libvxl_block));
		}
	}

	int sg = (w*h*d+7)/8;
	map->geometry = malloc(sg);
	if(data) {
		memset(map->geometry,0xFF,sg);
	} else {
		memset(map->geometry,0x00,sg);
		for(int y=0;y<h;y++)
			for(int x=0;x<w;x++)
				libvxl_geometry_set(map,x,y,d-1,1);
		return;
	}

	int offset = 0;
	for(int y=0;y<map->height;y++) {
		for(int x=0;x<map->width;x++) {

			int chunk_x = x/CHUNK_SIZE;
			int chunk_y = y/CHUNK_SIZE;
			struct libvxl_chunk* chunk = map->chunks+chunk_x+chunk_y*sx;

			while(1) {
				struct libvxl_span* desc = (struct libvxl_span*)(data+offset);
				int* color_data = (int*)(data+offset+sizeof(struct libvxl_span));
				struct libvxl_span* desc_next = (struct libvxl_span*)(data+offset+libvxl_span_length(desc));

				for(int z=desc->air_start;z<desc->color_start;z++)
					libvxl_geometry_set(map,x,y,z,0);

				for(int z=desc->color_start;z<=desc->color_end;z++) //top color run
					libvxl_chunk_put(chunk,pos_key(x,y,z),color_data[z-desc->color_start]);

				int top_len = desc->color_end-desc->color_start+1;
				int bottom_len = desc->length-1-top_len;

				if(desc->length>0) {
					for(int z=desc_next->air_start-bottom_len;z<desc_next->air_start;z++) //bottom color run
						libvxl_chunk_put(chunk,pos_key(x,y,z),color_data[z-(desc_next->air_start-bottom_len)+top_len]);
					offset += libvxl_span_length(desc);
				} else {
					offset += libvxl_span_length(desc);
					break;
				}
			}
		}
	}
}

void libvxl_write(struct libvxl_map* map, void* out, int* size) {
	int sx = (map->width+CHUNK_SIZE-1)/CHUNK_SIZE;
	int sy = (map->height+CHUNK_SIZE-1)/CHUNK_SIZE;

	int chunk_offsets[sx*sy];
	memset(chunk_offsets,0,sx*sy*sizeof(int));

	int offset = 0;
	//int blocks = 0;
	for(int y=0;y<map->height;y++) {
		for(int x=0;x<map->width;x++) {
			int co = (x/CHUNK_SIZE)+(y/CHUNK_SIZE)*sx;
			struct libvxl_chunk* chunk = map->chunks+co;

			int z = 0;
			while(1) {
				int top_start, top_end;
				int bottom_start, bottom_end;
				for(top_start=z;top_start<map->depth && !libvxl_geometry_get(map,x,y,top_start);top_start++);
				for(top_end=top_start;top_end<map->depth && libvxl_geometry_get(map,x,y,top_end) && libvxl_map_onsurface(map,x,y,top_end);top_end++);

				for(bottom_start=top_end;bottom_start<map->depth && libvxl_geometry_get(map,x,y,bottom_start) && !libvxl_map_onsurface(map,x,y,bottom_start);bottom_start++);
				for(bottom_end=bottom_start;bottom_end<map->depth && libvxl_geometry_get(map,x,y,bottom_end) && libvxl_map_onsurface(map,x,y,bottom_end);bottom_end++);

				struct libvxl_span* desc = (struct libvxl_span*)(out+offset);
				desc->color_start = top_start;
				desc->color_end = top_end-1;
				desc->air_start = z;
				offset += sizeof(struct libvxl_span);

				for(int k=top_start;k<top_end;k++) {
					*(int*)(out+offset) = chunk->blocks[chunk_offsets[co]++].color&0xFFFFFF|0x7F000000;
					offset += sizeof(int);
					//blocks++;
				}

				if(bottom_start==map->depth) {
					//this is the last span of this column, do not emit bottom colors, set length to 0
					desc->length = 0;
					break;
				} else {
					//there are more spans to follow, emit bottom colors
					if(bottom_end<map->depth) {
						desc->length = 1+top_end-top_start+bottom_end-bottom_start;
						for(int k=bottom_start;k<bottom_end;k++) {
							*(int*)(out+offset) = chunk->blocks[chunk_offsets[co]++].color&0xFFFFFF|0x7F000000;
							offset += sizeof(int);
							//blocks++;
						}
					} else {
						desc->length = 1+top_end-top_start;
					}
				}

				z = (bottom_end<map->depth)?bottom_end:bottom_start;
			}
		}
	}
	/*printf("blocks written: %i\n",blocks);

	int a = 0, b = 0, c = 0;
	for(int k=0;k<sx*sy;k++) {
		for(int i=0;i<map->chunks[k].index;i++) {
			int pos = map->chunks[k].blocks[i].position;
			int x = key_getx(pos);
			int y = key_gety(pos);
			int z = key_getz(pos);
			if(libvxl_map_onsurface(map,x,y,z))
				a++;
			else {
				b++;
				printf("non surface [%i,%i,%i]\n",x,y,z);
			}

			if(!libvxl_map_issolid(map,x,y,z))
				c++;
		}
	}

	printf("surface blocks: %i, others: %i, non-solids: %i\n",a,b,c);
	printf("setair on %i blocks\n",deleted);*/

	*size = offset;
}

void libvxl_writefile(struct libvxl_map* map, char* name) {
	FILE* f = fopen(name,"wb");
	void* mem = malloc(10*1024*1024);
	int size;
	libvxl_write(map,mem,&size);
	fwrite(mem,size,1,f);
	free(mem);
	fclose(f);
}


int libvxl_map_get(struct libvxl_map* map, int x, int y, int z) {
	if(x<0 || y<0 || z<0 || x>=map->width || y>=map->height || z>=map->depth)
		return 0;
	if(!libvxl_geometry_get(map,x,y,z))
		return 0;
	int chunk_cnt = (map->width+CHUNK_SIZE-1)/CHUNK_SIZE;
	int chunk_x = x/CHUNK_SIZE;
	int chunk_y = y/CHUNK_SIZE;
	struct libvxl_chunk* chunk = &map->chunks[chunk_x+chunk_y*chunk_cnt];
	struct libvxl_block blk;
	blk.position = pos_key(x,y,z);
	struct libvxl_block* loc = bsearch(&blk,chunk->blocks,chunk->index,sizeof(struct libvxl_block),cmp);
	return loc?loc->color:DEFAULT_COLOR;
}

int libvxl_map_issolid(struct libvxl_map* map, int x, int y, int z) {
	if(x<0 || y<0 || x>=map->width || y>=map->height || z>=map->depth)
		return 1;
	if(z<0)
		return 0;
	return libvxl_geometry_get(map,x,y,z)>0;
}

int libvxl_map_onsurface(struct libvxl_map* map, int x, int y, int z) {
	return !libvxl_map_issolid(map,x,y+1,z)
		|| !libvxl_map_issolid(map,x,y-1,z)
		|| !libvxl_map_issolid(map,x+1,y,z)
		|| !libvxl_map_issolid(map,x-1,y,z)
		|| !libvxl_map_issolid(map,x,y,z+1)
		|| !libvxl_map_issolid(map,x,y,z-1);
}

void libvxl_map_gettop(struct libvxl_map* map, int x, int y, int* result) {
	if(x<0 || y<0 || x>=map->width || y>=map->height)
		return;
	int z;
	for(z=0;z<map->depth;z++)
		if(libvxl_geometry_get(map,x,y,z))
			break;
	result[0] = libvxl_map_get(map,x,y,z);
	result[1] = z;
}

void libvxl_map_set_internal(struct libvxl_map* map, int x, int y, int z, int color) {
	if(x<0 || y<0 || z<0 || x>=map->width || y>=map->height || z>=map->depth)
		return;
	if(libvxl_geometry_get(map,x,y,z) && !libvxl_map_onsurface(map,x,y,z))
		return;
	int chunk_cnt = (map->width+CHUNK_SIZE-1)/CHUNK_SIZE;
	int chunk_x = x/CHUNK_SIZE;
	int chunk_y = y/CHUNK_SIZE;
	struct libvxl_chunk* chunk = map->chunks+chunk_x+chunk_y*chunk_cnt;
	libvxl_chunk_insert(chunk,pos_key(x,y,z),color);
}

void libvxl_map_setair_internal(struct libvxl_map* map, int x, int y, int z) {
	if(x<0 || y<0 || z<0 || x>=map->width || y>=map->height || z>=map->depth)
		return;
	if(!libvxl_geometry_get(map,x,y,z))
		return;
	int chunk_cnt = (map->width+CHUNK_SIZE-1)/CHUNK_SIZE;
	int chunk_x = x/CHUNK_SIZE;
	int chunk_y = y/CHUNK_SIZE;
	struct libvxl_chunk* chunk = &map->chunks[chunk_x+chunk_y*chunk_cnt];
	struct libvxl_block blk;
	blk.position = pos_key(x,y,z);
	void* loc = bsearch(&blk,chunk->blocks,chunk->index,sizeof(struct libvxl_block),cmp);
	if(loc) {
		int i = (loc-(void*)chunk->blocks)/sizeof(struct libvxl_block);
		memmove(loc,loc+sizeof(struct libvxl_block),(chunk->index-i-1)*sizeof(struct libvxl_block));
		chunk->index--;
	}
}

void libvxl_map_set(struct libvxl_map* map, int x, int y, int z, int color) {
	if(x<0 || y<0 || z<0 || x>=map->width || y>=map->height || z>=map->depth)
		return;
	libvxl_map_set_internal(map,x,y,z,color);
	libvxl_geometry_set(map,x,y,z,1);

	if(libvxl_map_issolid(map,x,y+1,z) && !libvxl_map_onsurface(map,x,y+1,z))
		libvxl_map_setair_internal(map,x,y+1,z);
	if(libvxl_map_issolid(map,x,y-1,z) && !libvxl_map_onsurface(map,x,y-1,z))
		libvxl_map_setair_internal(map,x,y-1,z);

	if(libvxl_map_issolid(map,x+1,y,z) && !libvxl_map_onsurface(map,x+1,y,z))
		libvxl_map_setair_internal(map,x+1,y,z);
	if(libvxl_map_issolid(map,x-1,y,z) && !libvxl_map_onsurface(map,x-1,y,z))
		libvxl_map_setair_internal(map,x-1,y,z);

	if(libvxl_map_issolid(map,x,y,z+1) && !libvxl_map_onsurface(map,x,y,z+1))
		libvxl_map_setair_internal(map,x,y,z+1);
	if(libvxl_map_issolid(map,x,y,z-1) && !libvxl_map_onsurface(map,x,y,z-1))
		libvxl_map_setair_internal(map,x,y,z-1);
}

void libvxl_map_setair(struct libvxl_map* map, int x, int y, int z) {
	if(x<0 || y<0 || z<0 || x>=map->width || y>=map->height || z>=map->depth)
		return;

	int surface_prev[6] = {
		libvxl_map_issolid(map,x,y+1,z)?libvxl_map_onsurface(map,x,y+1,z):1,
		libvxl_map_issolid(map,x,y-1,z)?libvxl_map_onsurface(map,x,y-1,z):1,
		libvxl_map_issolid(map,x+1,y,z)?libvxl_map_onsurface(map,x+1,y,z):1,
		libvxl_map_issolid(map,x-1,y,z)?libvxl_map_onsurface(map,x-1,y,z):1,
		libvxl_map_issolid(map,x,y,z+1)?libvxl_map_onsurface(map,x,y,z+1):1,
		libvxl_map_issolid(map,x,y,z-1)?libvxl_map_onsurface(map,x,y,z-1):1
	};

	libvxl_map_setair_internal(map,x,y,z);
	libvxl_geometry_set(map,x,y,z,0);

	if(!surface_prev[0] && libvxl_map_onsurface(map,x,y+1,z))
		libvxl_map_set_internal(map,x,y+1,z,DEFAULT_COLOR);
	if(!surface_prev[1] && libvxl_map_onsurface(map,x,y-1,z))
		libvxl_map_set_internal(map,x,y-1,z,DEFAULT_COLOR);

	if(!surface_prev[2] && libvxl_map_onsurface(map,x+1,y,z))
		libvxl_map_set_internal(map,x+1,y,z,DEFAULT_COLOR);
	if(!surface_prev[3] && libvxl_map_onsurface(map,x-1,y,z))
		libvxl_map_set_internal(map,x-1,y,z,DEFAULT_COLOR);

	if(!surface_prev[4] && libvxl_map_onsurface(map,x,y,z+1))
		libvxl_map_set_internal(map,x,y,z+1,DEFAULT_COLOR);
	if(!surface_prev[5] && libvxl_map_onsurface(map,x,y,z-1))
		libvxl_map_set_internal(map,x,y,z-1,DEFAULT_COLOR);
}
