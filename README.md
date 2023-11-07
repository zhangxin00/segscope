# segscope
This repository contains the experiments of evaluation and case studies discussed in the paper  
* "SegScope: Probing Fine-grained Interrupts via Architectural Footprints" (HPCA 2024).
  
SegScope can be used to probe interrupts without any timer. We successfully apply it to resurrect multiple end-to-end attacks in a timer-constrained scenario.

## Tested Setup

SegScope works across a wide range of Intel- and AMD-based CPUs.

All systems are running Ubuntu system (Linux kernel 5.x).


## Materials
This repository contains the following materials:

* `SegScope-based timer`: contains the code that we exploit timer interrupts to construct a novel timer.
* `Website Fingerprinting`: contains the code that we apply SegScope to detect interrupts while opening a website.
* `breaking KASLR`: contains the code that we derandomize KASLR.
* `enhancing Spectral attack`: contains the code that we use SegScope to enhance a non-interrupt side channel attack (i.e., [spectral](https://github.com/cispa/mwait) ).

## Contact
If there are questions regarding these experiments, please send an email to `zhangxin00@stu.pku.edu.cn`.
