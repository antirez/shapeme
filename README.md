Shapeme -- A mess of a program to evolve images using simulated annealing.
===

This is some code I wrote 6 years ago and never released, just after I saw [this insipring blog post of 2008](http://rogeralsing.com/2008/12/07/genetic-programming-evolution-of-mona-lisa/). I remember my son having tons of fun with this, so while I rarely touched the code at all, I ran it multiple times for him, and I plan to do it again when my daugher is old enough to understand it.

So what it does?
1. It takes a reference image (a PNG file).
2. It starts with a set of random triangles and/or circles.
3. It evolves this triangles via random mutations using some broken simulated annealing in order to evolve it into the final image.
4. The program saves / loads states.
5. It also outputs an SVG file representing the image, so you can print a big version of it. Sometimes generated pictures are quite cool.

Since it uses the SDL library, it shows visually the evolution of the image, and I believe this is what was so cool for my son to watch, and probably you'll have fun too.

In the release there is an example PNG you can use, the Annunziata, a painting authored by [Antonello da Messina](http://en.wikipedia.org/wiki/Antonello_da_Messina), a Sicilian painter.

Examples
---

This is what the program outputs with 64 triangles after a few minutes of evolution: http://antirez.com/misc/monnalisa.png

![Evolved Monnalisa](http://antirez.com/misc/monnalisa.png "64 triangles Monnalisa")

How to build?
---

There is a terrible Makefile. Try `make`... It needs:

1. The PNG lib.
2. SDL.

How to run the program?
---

Basic usage using the example image:

    ./shapeme annunziata.png /tmp/annunziata.bin /tmp/annunziata.svg

For additional options just run the program without args, it will print some help.

Have fun!
