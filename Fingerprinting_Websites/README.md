# Fingerprinting Websites

Uses SegScope to perform a classic interrupt side channel attack (i.e., website fingerprinting).

Install dependencies.
```
pip install requirements.txt
```

Record 20 5000-interrupt traces of the top 4 websites according to Alexa, and save the traces to segscope-experiment. Three absolute paths in record.py need to be modified in record_data.py.
```
python record_data.py --browser chrome --num_runs 20 **--attacker_type tick** --sites_list alexa4 --trace_length 5  --out_directory segscope-experiment
```

Load the traces and check accuracy
```
python scripts/check_results.py --data_file segscope-experiment
```

For more details, please refer to [ISCA 23](https://github.com/jackcook/bigger-fish)
