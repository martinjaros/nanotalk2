#!/usr/bin/env python

MESSAGES = True
TRACELOG = True
GRAPHS = False

import os, sys, time, subprocess

env1 = dict(os.environ)
env2 = env1

if MESSAGES:
    env1.update({'G_MESSAGES_DEBUG': 'all'})

if GRAPHS:
    env1.update({'GST_DEBUG_DUMP_DOT_DIR': '.'})
    subprocess.call('rm -f *-pipeline.dot.svg', shell=True)

if TRACELOG:
    env1 = dict(env1)
    env1.update({'GST_DEBUG_FILE': 'test1.log', 'GST_DEBUG': '*:WARNING,GST_INIT:INFO'})
    env2.update({'GST_DEBUG_FILE': 'test2.log', 'GST_DEBUG': '*:WARNING,GST_INIT:INFO'})

proc1 = subprocess.Popen('nanotalk -c test1.cfg -k test1.key -a aliases.txt'.split(), env=env1)
time.sleep(.1)
proc2 = subprocess.Popen('nanotalk -c test2.cfg -k test2.key -a aliases.txt'.split(), env=env2)

try:
    proc1.wait()
    proc2.wait()
except KeyboardInterrupt:
    sys.stdout.write('\n')

if GRAPHS:
    subprocess.call('dot -O -Tsvg *-pipeline.dot', shell=True)
    subprocess.call('rm -f *-pipeline.dot', shell=True)
