SegScope can be used to craft a fine-grained timing source. Specifically, given that different types of interrupts present distinguishable statistical characteristics, we can filter out timer interrupts and apply statistical methods such as Z-score. As fine-grained timer interrupts as clock edges contain timestamps, they can serve as a timer. We then leverage the timer to successfully break KASLR within about 10 seconds.

Run using ./main or bash run.sh. It outputs the granularity and stability.
