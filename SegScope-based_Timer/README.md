# SegScope can be used to craft a fine-grained timing source.

Build our code.
```
make
```

Run our code. It outputs the granularity and stability. In our paper, the granularity refers to the cost of CPU cycles for one increment of a counter. The stability is the degree to which our timer is affected by system noise. 
```
./main 
```

You can see our timer achieves the same granularity with rdtsc/rdpru.
