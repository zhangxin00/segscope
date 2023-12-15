# SegScope

This repository contains the experiments of evaluation and case studies discussed in the paper  
* "SegScope: Probing Fine-grained Interrupts via Architectural Footprints" (HPCA 2024).
  
SegScope can be used to probe interrupts without any timer. We successfully apply it to resurrect multiple end-to-end attacks in a timer-constrained scenario.

## Tested Setup

### Software dependencies

In order to run the experiments and proof-of-concepts, the following prerequisites need to be fulfilled:

* Linux installation
  * Build tools (gcc, make)
  * Python 3

* Browsers (for website fingerprinting)
  * [Chrome Browser](https://www.google.com/chrome/) or [Tor Browser](https://www.torproject.org/download/).
  * You also need to install the drivers for the browsers you would like to use.
  - Chrome: Download [here](https://chromedriver.chromium.org/downloads) and add `chromedriver` to your path
  - [Tor Browser](https://www.torproject.org): Install [tor-browser-selenium](https://github.com/webfp/tor-browser-selenium)

### Hardware dependencies

Throughout our experiments, we successfully evaluated our implementations on the following environments. We recommend to test SegScope on bare-metal machines.

| Machine                | CPU                  | Kernel          |
| ---------------------- | -------------------  | --------------- |
| Xiaomi Air 13.3        | Intel Core i5-8250U  | Linux 5.15.0    |
| Lenovo Yangtian 4900v  | Intel Core i7-4790   | Linux 5.8.0     |
| Lenovo Savior Y9000P   | Intel Core i9-12900H | Linux 5.15.0    |
| Honor Magicbook 16 Pro | AMD Ryzen 7 5800H    | Linux 5.15.0    |
| Amazon t2.large (Xen)  | Intel Xeon E5-2686   | Linux 5.15.0    |
| Amazon c5.large (KVM)  | Intel Xeon 8275CL    | Linux 5.15.0    |

 **Note:** The enhanced Spectral attack relies on the UMONITOR/UMWAIT instructions that are only available on Intel latest core processors (Tremont and Alder Lake). We evaluate it on our Lenovo Savior Y9000P machine. Please refer to [mwait](https://github.com/cispa/mwait) for more details.


## Materials

This repository contains the following materials:

* `E1-Website Fingerprinting`: contains the code that we apply SegScope to detect interrupts while opening a website.
* `E2-Enhancing Spectral attack`: contains the code that we use SegScope to enhance a non-interrupt side channel attack (i.e., [spectral](https://github.com/cispa/mwait) ).
* `E3-Breaking KASLR`: contains the code that we rely on SegScope-based timer to derandomize KASLR.

## Contact

If there are questions regarding these experiments, please send an email to `zhangxin00@stu.pku.edu.cn`.

## How should I cite this work?

Please use the following BibTeX entry:

```latex
@inproceedings{Zhang2024SegScope,
  year={2024},
  title={SegScope: Probing Fine-grained Interrupts via Architectural Footprints},
  booktitle={High Performance Computer Architecture},
  author={Xin Zhang and Zhi Zhang and Qingni Shen and Wenhao Wang and Yansong Gao and Zhuoxi Yang and Jiliang Zhang}
}
