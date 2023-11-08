# Breaking KASLR with SegScope-based Timer

Uses SegScope-based timer to perform a realistic timing side channel attack, which derandomizes KASLR. 

To build our code
```
make
```

Run our prefetch-based attack for 10 times.
```
./main 10 p
```

The program outputs the top-5 guess results. The measurements for all the 512 possible offsets are shown in `result-512`.

You can also perform access-based attack with our timer. Segment faults that will be incurred are dealt with by a pre-defined userspace handler.
```
./main 10 a
```

