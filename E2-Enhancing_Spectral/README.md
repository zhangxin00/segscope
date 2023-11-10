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

Evaluate the leakage rate for different umwait timeouts from 1000 to 200000. The result is stored in log.csv.
```
./test.sh
```

