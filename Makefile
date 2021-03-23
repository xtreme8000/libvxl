CFLAGS=-Wall -Wextra -pedantic -std=c99 -Ofast

all:
	${CC} -c libvxl.c -fPIC ${CFLAGS} -o libvxl.o
