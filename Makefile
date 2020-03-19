all: shapeme

shapeme: shapeme.c
	$(CC) -O3 shapeme.c `libpng-config --cflags` `libpng-config --L_opts` `libpng-config --libs` `sdl2-config --cflags` `sdl2-config --libs` -lm -o shapeme -Wall -W

clean:
	rm -f shapeme
