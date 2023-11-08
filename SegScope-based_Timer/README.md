# SegScope can be used to craft a fine-grained timing source.

Build our code.
```
make
```

Run our code. It outputs the granularity and stability.
```
./main 
```



```
./main 10 a
```

The ground truth can be accessed by privileged commonds. For multiple boots, the result of side channel attack should always be equal to the output value, or differ by 0x100000.
```
sudo cat /proc/kallsyms | grep _text | head -1
```
