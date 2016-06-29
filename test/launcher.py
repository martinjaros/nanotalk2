#!/usr/bin/env python

import os, sys, subprocess

os.environ['PULSE_PROP'] = 'filter.want=echo-cancel'
os.environ['G_MESSAGES_DEBUG'] = 'all'

#os.environ['GST_DEBUG'] = '*:WARNING'
#os.environ['GST_DEBUG_DUMP_DOT_DIR'] = '.'

os.environ['HOME'] = 'Test1'
proc1 = subprocess.Popen('../src/nanotalk')

os.environ['HOME'] = 'Test2'
proc2 = subprocess.Popen('../src/nanotalk')

try:
    proc1.wait()
    proc2.wait()
except KeyboardInterrupt:
    sys.stdout.write('\n')

