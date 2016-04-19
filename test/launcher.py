#!/usr/bin/env python

MESSAGES = True
TRACELOG = False
GRAPHS = False

import os, sys, time, subprocess

if MESSAGES:
    os.environ.update({'G_MESSAGES_DEBUG': 'all'})

if TRACELOG:
    os.environ.update({'GST_DEBUG_FILE': 'trace.log', 'GST_DEBUG': '*:WARNING,GST_INIT:INFO'})

if GRAPHS:
    os.environ.update({'GST_DEBUG_DUMP_DOT_DIR': '.'})
    subprocess.call('rm -f *-pipeline.dot.svg', shell=True)

proc1 = subprocess.Popen('nanotalk -c test1.cfg -k test1.key -a aliases.txt'.split(), env=os.environ)
time.sleep(.1)
proc2 = subprocess.Popen('nanotalk -c test2.cfg -k test2.key -a aliases.txt'.split(), env=os.environ)

try:
    proc1.wait()
    proc2.wait()
except KeyboardInterrupt:
    sys.stdout.write('\n')

if GRAPHS:
    subprocess.call('dot -O -Tsvg *-pipeline.dot', shell=True)
    subprocess.call('rm -f *-pipeline.dot', shell=True)
