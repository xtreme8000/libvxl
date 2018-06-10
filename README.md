## libvxl

* read n' write Ace of Spades map files **easily** and **fast**
* compressed internal format derived from .vxl
* supports enhanced features for special cases:
  * floating block detection
  * get top layer block, useful for map overviews

## Example

All functions use voxlap's coordinate system:

![coordsys](docs/coordsys.gif)

```C
//create map from v pointer
struct libvxl_map m;
libvxl_create(&m,512,512,64,v);

//get color at position (x,y,z)
int col = libvxl_map_get(&m,x,y,z);

//check if block is solid at (x,y,z)
int solid = libvxl_map_issolid(&m,x,y,z);

//free map after use
libvxl_free(&m);
```
