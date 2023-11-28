# Enhancing Spectral attack
[Spectral attack](https://github.com/cispa/mwait/tree/main/spectral) is badly noised by interrupts. However, SegScope can filter the noised measurements.

Build our code
```
make
```

Quick test for the attack. Every second it outputs the leakage rate, error rate, and true capacity. An optional parameter can be provided to stop the experiment after N seconds: ./main N. If this parameter is provided, it outputs the last leakage rate, error rate, and true capacity as CSVs.
```
./main
```

Expected results are as follows. The program outputs the error rate of original spectral(%), speed, channel_capacity, number of interrupted measuments, and the error rate of our **enhanced spectral**(%).
```
0.559,53889,51200.8,irq=750 ,after_filter:0.098
```


Evaluate the leakage rate for different umwait timeouts from 1000 to 200000. The result is stored in log.csv.
```
./test.sh
```

Expected results are as follows:
```
93000,0.513,57350,54690.3,irq=735 ,after_filter:0.099
94000,0.478,56791,54306.1,irq=689 ,after_filter:0.101
95000,0.415,56085,53908.9,irq=673 ,after_filter:0.099
96000,0.487,55597,53128.1,irq=728 ,after_filter:0.066
97000,0.485,54989,52555.1,irq=724 ,after_filter:0.065
98000,0.450,54529,52263.2,irq=673 ,after_filter:0.099
99000,0.455,54238,51961.2,irq=673 ,after_filter:0.098
100000,0.559,53889,51200.8,irq=750 ,after_filter:0.098
101000,0.561,52909,50263.6,irq=745 ,after_filter:0.098
102000,0.486,52575,50242.5,irq=719 ,after_filter:0.066
103000,0.540,51989,49468.4,irq=728 ,after_filter:0.096
104000,0.567,51780,49166.2,irq=741 ,after_filter:0.101
105000,0.548,51431,48907.2,irq=748 ,after_filter:0.064
```
