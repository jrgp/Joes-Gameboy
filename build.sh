exec cc gb.c -Wall -g -pedantic -Wshadow -Wpointer-arith -Wcast-qual  `pkg-config sdl2 --cflags --libs` -o gb
