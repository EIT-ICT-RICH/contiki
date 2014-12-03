import os
import sys
import re
import fileinput
import math
import parseLogs

def extract(dir):
    
    print "Looping over all experiments"
    for file in os.listdir(dir):
        path = (os.path.join(dir, file))
        print path,
        sys.stdout.flush()
        if not os.path.exists(os.path.join(path, 'log.txt')):
        	print " not an experiment directory."
        	continue
        if os.path.exists(os.path.join(path, 'ongoing')):
        	print " is ongoing"
        	continue
        if os.path.exists(os.path.join(path, 'plots/allplots.pdf')):
            print " already done."
            continue
        print " extracting data...",
        sys.stdout.flush() 
        os.system("python extractFromTrace.py %s > /dev/null" %path)
        print " generating plots...",
        sys.stdout.flush()
        os.system("python generateSummaryPlots.py %s > /dev/null" %path)
        print " done."

extract('experiments/')
