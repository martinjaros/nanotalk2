#!/usr/bin/env python

import os, sys, subprocess

os.environ['G_MESSAGES_DEBUG'] = 'all'
os.environ['GST_DEBUG'] = '*:WARNING,GST_INIT:INFO'
#os.environ['GST_DEBUG_DUMP_DOT_DIR'] = '.'

os.environ['HOME'] = 'Test1'
os.environ['GST_DEBUG_FILE'] = 'test1.log'
proc1 = subprocess.Popen('../src/nanotalk')

os.environ['HOME'] = 'Test2'
os.environ['GST_DEBUG_FILE'] = 'test2.log'
proc2 = subprocess.Popen('../src/nanotalk')

try:
    proc1.wait()
    proc2.wait()
except KeyboardInterrupt:
    sys.stdout.write('\n')

