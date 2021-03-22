#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "libvxl.h"

static uint32_t libvxl_geometry_get(struct libvxl_map* map, uint32_t x,
									uint32_t y, uint32_t z) {
	size_t offset = z + (x + y * map->width) * map->depth;
	return map->geometry[offset / (sizeof(uint32_t) * 8)]
		& (1 << (offset % (sizeof(uint32_t) * 8)));
}

static void libvxl_geometry_set(struct libvxl_map* map, uint32_t x, uint32_t y,
								uint32_t z, uint32_t state) {
	if(x >= map->width || y >= map->height || z >= map->depth)
		return;
	size_t offset = z + (x + y * map->width) * map->depth;

	uint32_t* val = map->geometry + offset / (sizeof(uint32_t) * 8);
	uint32_t bit = offset % (sizeof(uint32_t) * 8);

	*val = (*val & ~(1 << bit)) | (state << bit);
}

static int cmp(const void* a, const void* b) {
	struct libvxl_block* aa = (struct libvxl_block*)a;
	struct libvxl_block* bb = (struct libvxl_block*)b;
	return aa->position - bb->position;
}

static void libvxl_chunk_put(struct libvxl_chunk* chunk, uint32_t pos,
							 uint32_t color) {
	if(chunk->index == chunk->length) { // needs to grow
		chunk->length += LIBVXL_CHUNK_GROWTH;
		chunk->blocks = realloc(chunk->blocks,
								chunk->length * sizeof(struct libvxl_block));
	}
	struct libvxl_block blk;
	blk.position = pos;
	blk.color = color;
	memcpy(&chunk->blocks[chunk->index++], &blk, sizeof(struct libvxl_block));
}

static void libvxl_chunk_insert(struct libvxl_chunk* chunk, uint32_t pos,
								uint32_t color) {
	int start = 0;
	int end = chunk->index;
	while(end - start > 0) {
		int mid = (start + end) / 2;
		int diff = pos - chunk->blocks[mid].position;
		if(diff > 0) {
			start = mid + 1;
		} else if(diff < 0) {
			end = mid;
		} else { // diff=0, replace color
			chunk->blocks[mid].color = color;
			return;
		}
	}

	if(chunk->index == chunk->length) { // needs to grow
		chunk->length += LIBVXL_CHUNK_GROWTH;
		chunk->blocks = realloc(chunk->blocks,
								chunk->length * sizeof(struct libvxl_block));
	}

	memmove(chunk->blocks + start + 1, chunk->blocks + start,
			(chunk->index - start) * sizeof(struct libvxl_block));
	chunk->blocks[start].position = pos;
	chunk->blocks[start].color = color;
	chunk->index++;
}

static int libvxl_span_length(struct libvxl_span* s) {
	return s->length > 0 ? s->length * 4 :
						   (s->color_end - s->color_start + 2) * 4;
}

void libvxl_free(struct libvxl_map* map) {
	if(!map)
		return;
	int sx = (map->width + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE;
	int sy = (map->height + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE;
	for(int k = 0; k < sx * sy; k++)
		free(map->chunks[k].blocks);
	free(map->chunks);
	free(map->geometry);
}

int libvxl_size(int* size, int* depth, const void* data, size_t len) {
	if(!data)
		return 0;
	int offset = 0;
	int columns = 0;
	while(offset + sizeof(struct libvxl_span) - 1 < len) {
		struct libvxl_span* desc = (struct libvxl_span*)(data + offset);
		if(desc->color_end + 1 > *depth)
			*depth = desc->color_end + 1;
		if(!desc->length)
			columns++;
		offset += libvxl_span_length(desc);
	}
	*size = sqrt(columns);
	return 1;
}

int libvxl_create(struct libvxl_map* map, int w, int h, int d, const void* data,
				  size_t len) {
	if(!map)
		return 0;
	map->streamed = 0;
	map->width = w;
	map->height = h;
	map->depth = d;
	int sx = (w + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE;
	int sy = (h + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE;
	map->chunks = malloc(sx * sy * sizeof(struct libvxl_chunk));
	for(int y = 0; y < sy; y++) {
		for(int x = 0; x < sx; x++) {
			map->chunks[x + y * sx].length = LIBVXL_CHUNK_SIZE
				* LIBVXL_CHUNK_SIZE * 2; // allows for two fully filled layers
			map->chunks[x + y * sx].index = 0;
			map->chunks[x + y * sx].blocks = malloc(
				map->chunks[x + y * sx].length * sizeof(struct libvxl_block));
		}
	}

	map->queue.index = 0;
	map->queue.length = 64;
	map->queue.blocks = malloc(map->queue.length * sizeof(struct libvxl_block));

	size_t sg = (w * h * d + (sizeof(uint32_t) * 8 - 1))
		/ (sizeof(uint32_t) * 8) * sizeof(uint32_t);
	map->geometry = malloc(sg);
	if(data) {
		memset(map->geometry, 0xFF, sg);
	} else {
		memset(map->geometry, 0x00, sg);
		for(int y = 0; y < h; y++)
			for(int x = 0; x < w; x++)
				libvxl_map_set(map, x, y, d - 1, DEFAULT_COLOR(x, y, d - 1));
		return 1;
	}

	size_t offset = 0;
	for(uint32_t y = 0; y < map->height; y++) {
		for(uint32_t x = 0; x < map->width; x++) {
			int chunk_x = x / LIBVXL_CHUNK_SIZE;
			int chunk_y = y / LIBVXL_CHUNK_SIZE;
			struct libvxl_chunk* chunk = map->chunks + chunk_x + chunk_y * sx;

			while(1) {
				if(offset + sizeof(struct libvxl_span) - 1 >= len)
					return 0;
				struct libvxl_span* desc = (struct libvxl_span*)(data + offset);
				if(offset + libvxl_span_length(desc) - 1 >= len)
					return 0;
				uint32_t* color_data
					= (uint32_t*)(data + offset + sizeof(struct libvxl_span));

				for(int z = desc->air_start; z < desc->color_start; z++)
					libvxl_geometry_set(map, x, y, z, 0);

				for(int z = desc->color_start; z <= desc->color_end;
					z++) // top color run
					libvxl_chunk_put(chunk, pos_key(x, y, z),
									 color_data[z - desc->color_start]);

				int top_len = desc->color_end - desc->color_start + 1;
				int bottom_len = desc->length - 1 - top_len;

				if(desc->length > 0) {
					if(offset + libvxl_span_length(desc)
						   + sizeof(struct libvxl_span) - 1
					   >= len)
						return 0;
					struct libvxl_span* desc_next
						= (struct libvxl_span*)(data + offset
												+ libvxl_span_length(desc));
					for(int z = desc_next->air_start - bottom_len;
						z < desc_next->air_start; z++) // bottom color run
						libvxl_chunk_put(
							chunk, pos_key(x, y, z),
							color_data[z - (desc_next->air_start - bottom_len)
									   + top_len]);
					offset += libvxl_span_length(desc);
				} else {
					offset += libvxl_span_length(desc);
					break;
				}
			}
		}
	}

	return 1;
}

static void libvxl_column_encode(struct libvxl_map* map, size_t* chunk_offsets,
								 int x, int y, void* out, size_t* offset) {
	int sx = (map->width + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE;
	int co = (x / LIBVXL_CHUNK_SIZE) + (y / LIBVXL_CHUNK_SIZE) * sx;
	struct libvxl_chunk* chunk = map->chunks + co;

	int z = key_getz(chunk->blocks[chunk_offsets[co]].position);
	while(1) {
		uint32_t top_start, top_end;
		uint32_t bottom_start, bottom_end;
		for(top_start = z; top_start < map->depth
			&& !libvxl_geometry_get(map, x, y, top_start);
			top_start++)
			;
		for(top_end = top_start;
			top_end < map->depth && libvxl_geometry_get(map, x, y, top_end)
			&& libvxl_map_onsurface(map, x, y, top_end);
			top_end++)
			;

		for(bottom_start = top_end; bottom_start < map->depth
			&& libvxl_geometry_get(map, x, y, bottom_start)
			&& !libvxl_map_onsurface(map, x, y, bottom_start);
			bottom_start++)
			;
		for(bottom_end = bottom_start; bottom_end < map->depth
			&& libvxl_geometry_get(map, x, y, bottom_end)
			&& libvxl_map_onsurface(map, x, y, bottom_end);
			bottom_end++)
			;

		struct libvxl_span* desc = (struct libvxl_span*)(out + *offset);
		desc->color_start = top_start;
		desc->color_end = top_end - 1;
		desc->air_start = z;
		*offset += sizeof(struct libvxl_span);

		for(uint32_t k = top_start; k < top_end; k++) {
			*(int*)(out + *offset)
				= (chunk->blocks[chunk_offsets[co]++].color & 0xFFFFFF)
				| 0x7F000000;
			*offset += sizeof(int);
		}

		if(bottom_start == map->depth) {
			/* this is the last span of this column, do not emit bottom colors,
			 * set length to 0 */
			desc->length = 0;
			break;
		} else {
			// there are more spans to follow, emit bottom colors
			if(bottom_end < map->depth) {
				desc->length
					= 1 + top_end - top_start + bottom_end - bottom_start;
				for(uint32_t k = bottom_start; k < bottom_end; k++) {
					*(int*)(out + *offset)
						= (chunk->blocks[chunk_offsets[co]++].color & 0xFFFFFF)
						| 0x7F000000;
					*offset += sizeof(int);
				}
			} else {
				desc->length = 1 + top_end - top_start;
			}
		}

		z = (bottom_end < map->depth) ? bottom_end : bottom_start;
	}
}

void libvxl_stream(struct libvxl_stream* stream, struct libvxl_map* map,
				   int chunk_size) {
	if(!stream || !map || chunk_size == 0)
		return;
	stream->map = map;
	map->streamed++;
	stream->chunk_size = chunk_size;
	stream->pos = pos_key(0, 0, 0);
	stream->buffer_offset = 0;
	stream->buffer = malloc(stream->chunk_size * 2);

	int sx = (map->width + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE;
	int sy = (map->height + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE;
	stream->chunk_offsets = malloc(sx * sy * sizeof(size_t));
	memset(stream->chunk_offsets, 0, sx * sy * sizeof(size_t));
}

void libvxl_stream_free(struct libvxl_stream* stream) {
	if(!stream)
		return;
	stream->map->streamed--;
	free(stream->buffer);
	free(stream->chunk_offsets);
}

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

int libvxl_stream_read(struct libvxl_stream* stream, void* out) {
	if(!stream || !out || key_gety(stream->pos) >= stream->map->height)
		return 0;
	while(stream->buffer_offset < stream->chunk_size
		  && key_gety(stream->pos) < stream->map->height) {
		libvxl_column_encode(stream->map, stream->chunk_offsets,
							 key_getx(stream->pos), key_gety(stream->pos),
							 stream->buffer, &stream->buffer_offset);
		if(key_getx(stream->pos) + 1 < stream->map->width)
			stream->pos
				= pos_key(key_getx(stream->pos) + 1, key_gety(stream->pos), 0);
		else
			stream->pos = pos_key(0, key_gety(stream->pos) + 1, 0);
	}
	size_t length = stream->buffer_offset;
	memcpy(out, stream->buffer, min(length, stream->chunk_size));
	if(length < stream->chunk_size) {
		stream->buffer_offset = 0;
	} else {
		memmove(stream->buffer, stream->buffer + stream->chunk_size,
				length - stream->chunk_size);
		stream->buffer_offset -= stream->chunk_size;
	}
	return min(length, stream->chunk_size);
}

void libvxl_write(struct libvxl_map* map, void* out, int* size) {
	if(!map || !out)
		return;
	int sx = (map->width + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE;
	int sy = (map->height + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE;

	size_t chunk_offsets[sx * sy];
	memset(chunk_offsets, 0, sx * sy * sizeof(size_t));

	size_t offset = 0;
	for(uint32_t y = 0; y < map->height; y++)
		for(uint32_t x = 0; x < map->width; x++)
			libvxl_column_encode(map, chunk_offsets, x, y, out, &offset);

	if(size)
		*size = offset;
}

int libvxl_writefile(struct libvxl_map* map, char* name) {
	if(!map || !name)
		return 0;
	char buf[1024];
	struct libvxl_stream s;
	libvxl_stream(&s, map, 1024);
	FILE* f = fopen(name, "wb");
	int read, total = 0;
	while((read = libvxl_stream_read(&s, buf))) {
		fwrite(buf, 1, read, f);
		total += read;
	}
	fclose(f);
	libvxl_stream_free(&s);
	return total;
}

void libvxl_kv6_write(struct libvxl_map* map, char* name) {
	FILE* f = fopen(name, "wb");

	struct libvxl_kv6 header;
	strcpy(header.magic, "Kvxl");
	header.width = map->width;
	header.height = map->height;
	header.depth = map->depth;
	header.pivot[0] = map->width / 2.0F;
	header.pivot[1] = map->height / 2.0F;
	header.pivot[2] = map->depth / 2.0F;

	header.len = 0;

	int sx = (map->width + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE;
	int sy = (map->height + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE;
	for(int y = 0; y < sy; y++)
		for(int x = 0; x < sx; x++)
			header.len += map->chunks[x + y * sx].index;

	fwrite(&header, sizeof(header), 1, f);

	int xoffset[map->width];
	short xyoffset[map->width * map->height];
	memset(xoffset, 0, sizeof(xoffset));
	memset(xyoffset, 0, sizeof(xyoffset));

	size_t chunk_offsets[sx * sy];
	memset(chunk_offsets, 0, sizeof(chunk_offsets));

	for(uint32_t y = 0; y < map->height; y++) {
		for(uint32_t x = 0; x < map->width; x++) {
			int sx = (map->width + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE;
			int co = (x / LIBVXL_CHUNK_SIZE) + (y / LIBVXL_CHUNK_SIZE) * sx;
			struct libvxl_chunk* chunk = map->chunks + co;

			while(1) {
				struct libvxl_block* next = &chunk->blocks[chunk_offsets[co]];
				if(key_getx(next->position) != x)
					break;

				chunk_offsets[co]++;

				struct libvxl_kv6_block blk;
				blk.color = next->color & 0xFFFFFF;
				blk.z = key_getz(next->position);
				blk.normal = 0;

				blk.visfaces = 0;
				if(libvxl_map_issolid(map, x + 1, y, blk.z))
					blk.visfaces |= 0x01;
				if(libvxl_map_issolid(map, x - 1, y, blk.z))
					blk.visfaces |= 0x02;
				if(libvxl_map_issolid(map, x, y + 1, blk.z))
					blk.visfaces |= 0x04;
				if(libvxl_map_issolid(map, x, y - 1, blk.z))
					blk.visfaces |= 0x08;
				if(libvxl_map_issolid(map, x, y, blk.z + 1))
					blk.visfaces |= 0x10;
				if(libvxl_map_issolid(map, x, y, blk.z - 1))
					blk.visfaces |= 0x20;

				fwrite(&blk, sizeof(blk), 1, f);

				xoffset[x]++;
				xyoffset[x + y * map->width]++;
			}
		}
	}

	fwrite(xoffset, sizeof(xoffset), 1, f);
	fwrite(xyoffset, sizeof(xyoffset), 1, f);

	fclose(f);
}

int libvxl_map_isinside(struct libvxl_map* map, int x, int y, int z) {
	return map && x >= 0 && y >= 0 && z >= 0 && x < map->width
		&& y < map->height && z < map->depth;
}

uint32_t libvxl_map_get(struct libvxl_map* map, int x, int y, int z) {
	if(!map || x < 0 || y < 0 || z < 0 || x >= map->width || y >= map->height
	   || z >= map->depth)
		return 0;
	if(!libvxl_geometry_get(map, x, y, z))
		return 0;
	int chunk_cnt = (map->width + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE;
	int chunk_x = x / LIBVXL_CHUNK_SIZE;
	int chunk_y = y / LIBVXL_CHUNK_SIZE;
	struct libvxl_chunk* chunk = &map->chunks[chunk_x + chunk_y * chunk_cnt];
	struct libvxl_block blk;
	blk.position = pos_key(x, y, z);
	struct libvxl_block* loc = bsearch(&blk, chunk->blocks, chunk->index,
									   sizeof(struct libvxl_block), cmp);
	return loc ? loc->color : DEFAULT_COLOR(x, y, z);
}

int libvxl_map_issolid(struct libvxl_map* map, int x, int y, int z) {
	if(!map || x < 0 || y < 0 || x >= map->width || y >= map->height
	   || z >= map->depth)
		return 1;
	if(z < 0)
		return 0;
	return libvxl_geometry_get(map, x, y, z) > 0;
}

int libvxl_map_onsurface(struct libvxl_map* map, int x, int y, int z) {
	if(!map)
		return 0;
	return !libvxl_map_issolid(map, x, y + 1, z)
		|| !libvxl_map_issolid(map, x, y - 1, z)
		|| !libvxl_map_issolid(map, x + 1, y, z)
		|| !libvxl_map_issolid(map, x - 1, y, z)
		|| !libvxl_map_issolid(map, x, y, z + 1)
		|| !libvxl_map_issolid(map, x, y, z - 1);
}

void libvxl_map_gettop(struct libvxl_map* map, int x, int y, uint32_t* result) {
	if(!map || x < 0 || y < 0 || x >= map->width || y >= map->height)
		return;
	int z;
	for(z = 0; z < map->depth; z++)
		if(libvxl_geometry_get(map, x, y, z))
			break;
	result[0] = libvxl_map_get(map, x, y, z);
	result[1] = z;
}

static void libvxl_map_set_internal(struct libvxl_map* map, int x, int y, int z,
									uint32_t color) {
	if(x < 0 || y < 0 || z < 0 || x >= map->width || y >= map->height
	   || z >= map->depth)
		return;
	if(libvxl_geometry_get(map, x, y, z) && !libvxl_map_onsurface(map, x, y, z))
		return;
	int chunk_cnt = (map->width + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE;
	int chunk_x = x / LIBVXL_CHUNK_SIZE;
	int chunk_y = y / LIBVXL_CHUNK_SIZE;
	struct libvxl_chunk* chunk = map->chunks + chunk_x + chunk_y * chunk_cnt;
	libvxl_chunk_insert(chunk, pos_key(x, y, z), color);
}

static void libvxl_map_setair_internal(struct libvxl_map* map, int x, int y,
									   int z) {
	if(x < 0 || y < 0 || z < 0 || x >= map->width || y >= map->height
	   || z >= map->depth)
		return;
	if(!libvxl_geometry_get(map, x, y, z))
		return;
	int chunk_cnt = (map->width + LIBVXL_CHUNK_SIZE - 1) / LIBVXL_CHUNK_SIZE;
	int chunk_x = x / LIBVXL_CHUNK_SIZE;
	int chunk_y = y / LIBVXL_CHUNK_SIZE;
	struct libvxl_chunk* chunk = &map->chunks[chunk_x + chunk_y * chunk_cnt];
	struct libvxl_block blk;
	blk.position = pos_key(x, y, z);
	void* loc = bsearch(&blk, chunk->blocks, chunk->index,
						sizeof(struct libvxl_block), cmp);
	if(loc) {
		int i = (loc - (void*)chunk->blocks) / sizeof(struct libvxl_block);
		memmove(loc, loc + sizeof(struct libvxl_block),
				(chunk->index - i - 1) * sizeof(struct libvxl_block));
		chunk->index--;
		if(chunk->index <= chunk->length - LIBVXL_CHUNK_GROWTH * 2) {
			chunk->length -= LIBVXL_CHUNK_GROWTH;
			chunk->blocks = realloc(
				chunk->blocks, chunk->length * sizeof(struct libvxl_block));
		}
	}
}

void libvxl_map_set(struct libvxl_map* map, int x, int y, int z,
					uint32_t color) {
	if(!map || x < 0 || y < 0 || z < 0 || x >= map->width || y >= map->height
	   || z >= map->depth)
		return;

	libvxl_map_set_internal(map, x, y, z, color);
	libvxl_geometry_set(map, x, y, z, 1);

	if(libvxl_map_issolid(map, x, y + 1, z)
	   && !libvxl_map_onsurface(map, x, y + 1, z))
		libvxl_map_setair_internal(map, x, y + 1, z);
	if(libvxl_map_issolid(map, x, y - 1, z)
	   && !libvxl_map_onsurface(map, x, y - 1, z))
		libvxl_map_setair_internal(map, x, y - 1, z);

	if(libvxl_map_issolid(map, x + 1, y, z)
	   && !libvxl_map_onsurface(map, x + 1, y, z))
		libvxl_map_setair_internal(map, x + 1, y, z);
	if(libvxl_map_issolid(map, x - 1, y, z)
	   && !libvxl_map_onsurface(map, x - 1, y, z))
		libvxl_map_setair_internal(map, x - 1, y, z);

	if(libvxl_map_issolid(map, x, y, z + 1)
	   && !libvxl_map_onsurface(map, x, y, z + 1))
		libvxl_map_setair_internal(map, x, y, z + 1);
	if(libvxl_map_issolid(map, x, y, z - 1)
	   && !libvxl_map_onsurface(map, x, y, z - 1))
		libvxl_map_setair_internal(map, x, y, z - 1);
}

void libvxl_map_setair(struct libvxl_map* map, int x, int y, int z) {
	if(!map || x < 0 || y < 0 || z < 0 || x >= map->width || y >= map->height
	   || z >= map->depth)
		return;

	int surface_prev[6] = {
		libvxl_map_issolid(map, x, y + 1, z) ?
			libvxl_map_onsurface(map, x, y + 1, z) :
			1,
		libvxl_map_issolid(map, x, y - 1, z) ?
			libvxl_map_onsurface(map, x, y - 1, z) :
			1,
		libvxl_map_issolid(map, x + 1, y, z) ?
			libvxl_map_onsurface(map, x + 1, y, z) :
			1,
		libvxl_map_issolid(map, x - 1, y, z) ?
			libvxl_map_onsurface(map, x - 1, y, z) :
			1,
		libvxl_map_issolid(map, x, y, z + 1) ?
			libvxl_map_onsurface(map, x, y, z + 1) :
			1,
		libvxl_map_issolid(map, x, y, z - 1) ?
			libvxl_map_onsurface(map, x, y, z - 1) :
			1,
	};

	libvxl_map_setair_internal(map, x, y, z);
	libvxl_geometry_set(map, x, y, z, 0);

	if(!surface_prev[0] && libvxl_map_onsurface(map, x, y + 1, z))
		libvxl_map_set_internal(map, x, y + 1, z, DEFAULT_COLOR(x, y + 1, z));
	if(!surface_prev[1] && libvxl_map_onsurface(map, x, y - 1, z))
		libvxl_map_set_internal(map, x, y - 1, z, DEFAULT_COLOR(x, y - 1, z));

	if(!surface_prev[2] && libvxl_map_onsurface(map, x + 1, y, z))
		libvxl_map_set_internal(map, x + 1, y, z, DEFAULT_COLOR(x + 1, y, z));
	if(!surface_prev[3] && libvxl_map_onsurface(map, x - 1, y, z))
		libvxl_map_set_internal(map, x - 1, y, z, DEFAULT_COLOR(x - 1, y, z));

	if(!surface_prev[4] && libvxl_map_onsurface(map, x, y, z + 1))
		libvxl_map_set_internal(map, x, y, z + 1, DEFAULT_COLOR(x, y, z + 1));
	if(!surface_prev[5] && libvxl_map_onsurface(map, x, y, z - 1))
		libvxl_map_set_internal(map, x, y, z - 1, DEFAULT_COLOR(x, y, z - 1));
}
