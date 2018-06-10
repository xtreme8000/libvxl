struct libvxl_span {
    unsigned char length;
    unsigned char color_start;
    unsigned char color_end;
    unsigned char air_start;
};

struct libvxl_column {
    int length;
    int index;
    void* spans;
};

struct libvxl_map {
    int width;
    int height;
	int depth;
    struct libvxl_column* columns;
};

void libvxl_create(struct libvxl_map* map, int w, int h, int d, void* data);
int libvxl_map_issolid(struct libvxl_map* map, int x, int y, int z);
int libvxl_map_get(struct libvxl_map* map, int x, int y, int z);
int libvxl_map_gettop(struct libvxl_map* map, int x, int y);
void libvxl_free(struct libvxl_map* map);
