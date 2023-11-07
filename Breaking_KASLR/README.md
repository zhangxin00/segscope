# breaking KASLR with SegScope-based Timer

Uses SegScope-based timer to perform a realistic timing side channel attack, which derandomizes KASLR. 

Run using `./main 10 p`. It outputs the top-5 guess results. The measurements for all the 512 possible offsets are shown in `result-512`.

`./main 10 a` uses memory accesses to carry out the same attack. Segment faults that will be incurred are dealt with by a pre-defined userspace handler.
