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

execpath = '../src/nanotalk'
args1 = 'nanotalk -s incoming-call.ogg -k test1.key -a aliases.txt'
args2 = 'nanotalk -s incoming-call.ogg -k test2.key -a aliases.txt -l 5005 -h localhost'

proc1 = subprocess.Popen(args1.split(), executable=execpath, env=os.environ)
time.sleep(.1)
proc2 = subprocess.Popen(args2.split(), executable=execpath, env=os.environ)

try:
    proc1.wait()
    proc2.wait()
except KeyboardInterrupt:
    print

if GRAPHS:
    subprocess.call('dot -O -Tsvg *-pipeline.dot', shell=True)
    subprocess.call('rm -f *-pipeline.dot', shell=True)
