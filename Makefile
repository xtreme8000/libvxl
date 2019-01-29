CFLAGS=-Wall -Wextra -pedantic -std=c99

all:
	${CC} -c libvxl.c -fPIC ${CFLAGS} -o libvxl.o
