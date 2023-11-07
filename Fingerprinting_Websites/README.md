We modify the open-source code of [ISCA 23](https://github.com/jackcook/bigger-fish) to perform our website fingerprinting attack (--attacker_type tick). Please follow their setup. Three absolute paths need to be modified in record_data.py.

# Run using:

python record_data.py --browser chrome --num_runs 20 **--attacker_type tick** --sites_list alexa4 --trace_length 5  --out_directory segscope-experiment

python scripts/check_results.py --data_file segscope-experiment

