all: shapeme

shapeme: shapeme.c
	$(CC) -O3 shapeme.c `libpng-config --cflags` `libpng-config --L_opts` `libpng-config --libs` `sdl-config --cflags` `sdl-config --libs` -lm -o shapeme -Wall -W

clean:
	rm -f shapeme
