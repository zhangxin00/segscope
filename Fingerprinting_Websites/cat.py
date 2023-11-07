import pickle
import sys
import matplotlib.pyplot as plt
import numpy as np
import argparse
import pickle
import os
import warnings
path=sys.argv[1]
print(path)
if os.path.isdir(path):
        # If directory, find all .pkl files
        filepaths = [os.path.join(path, x) for x in os.listdir(path) if x.endswith(".pkl")]
elif os.path.isfile(path):
        # If single file, just use this one
        filepaths = [path]
else:
        raise RuntimeError

for file in filepaths:
        f = open(file, "rb")
        i=0

        while True:
            try:
                data = pickle.load(f)
                traces_i, labels_i = data[0], data[1]
                traces_i=np.array(traces_i)
                x=np.arange(0,traces_i.shape[1],1)
                plt.scatter(x,traces_i,s=2)
                plt.savefig(path+'/'+labels_i[12:]+str(i)+'.jpg')
                i+=1;
            except EOFError:
                break

    
