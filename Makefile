all: perfspurious

perfspurious: perfspurious.c
	gcc -std=gnu99 perfspurious.c -o perfspurious
