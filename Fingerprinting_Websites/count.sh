gcc -c -fPIC count-full.c
gcc -shared count-full.o -o count-full.so
