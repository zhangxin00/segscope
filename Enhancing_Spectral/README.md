[Spectral attack](https://github.com/cispa/mwait/tree/main/spectral) is badly noised by interrupts. However, SegScope can filter the noised measurements.

The below steps are same as Spectral.

Run using ./main. Every second it outputs the leakage rate, error rate, and true capacity. An optional parameter can be provided to stop the experiment after N seconds: ./main N. If this parameter is provided, it outputs the last leakage rate, error rate, and true capacity as CSVs.

Run ./test.sh to evaluate the leakage rate for different umwait timeouts from 1000 to 200000. The result is stored in log.csv.
