# Breaking KASLR with SegScope-based Timer

Uses SegScope-based timer to perform a realistic timing side channel attack, which derandomizes KASLR. 

Build our code
```
make
```

Run our timer. It outputs the granularity and stability. In our paper, the granularity refers to the cost of CPU cycles for one increment of a counter. The stability is the degree to which our timer is affected by system noise.  You can see our timer achieves the same granularity with rdtsc/rdpru.
```
./timer
```

Run our prefetch-based attack for 10 times. The program outputs the top-5 guess results. The measurements for all the 512 possible offsets are shown in `result-512`.
```
./main 10 p
```

You can also perform access-based attack with our timer. Segment faults that will be incurred are dealt with by a pre-defined userspace handler.
```
./main 10 a
```

The ground truth can be accessed by privileged commonds. For multiple boots, the result of side channel attack should always be equal to the output value, or differ by 0x100000.
```
sudo cat /proc/kallsyms | grep _text | head -1
```

