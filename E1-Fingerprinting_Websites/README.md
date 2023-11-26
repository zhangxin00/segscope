# Fingerprinting Websites

Uses SegScope to perform a classic interrupt side channel attack (i.e., website fingerprinting).

Install dependencies.
```
pip install -r requirements.txt
```

Build the .so file.
```
gcc -c -fPIC -o tick.o tick.c
gcc -shared tick.o -o tick.so
```

Record 20 5000-interrupt traces of the top 4 websites according to Alexa, and save the traces to segscope-experiment. Three absolute paths in record_data.py need to be modified in record_data.py.
```
python record_data.py --browser chrome --num_runs 20 --attacker_type tick --sites_list alexa4 --trace_length 5  --out_directory segscope-experiment
```

Load the traces and check accuracy.
```
python scripts/check_results.py --data_file segscope-experiment
```

Expected results are as follows:
```
python record_data.py --browser chrome --num_runs 20 --attacker_type tick --sites_list alexa4 --trace_length 5  --out_directory segscope-experiment

100%|█████████████████████████████████████████| 160/160 [43:10<00:00,  16.16s/it]

python scripts/check_results.py --data_file segscope-experiment

Analyzing results from readme-experiment
100%|███████████████████████████████████████████| 10/10 [00:01<00:00,  5.61it/s]

Number of traces: 160

top1 accuracy: 85.3% (+/- 5.5%)
top5 accuracy: 100.0% (+/- 0.0%)
```

For more details, please refer to [ISCA 23](https://github.com/jackcook/bigger-fish)
