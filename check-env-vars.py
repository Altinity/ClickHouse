#!/usr/bin/env python3
import sys
import subprocess

print("run")
subprocess.run("printenv", shell=True, stderr=sys.stdout, stdout=sys.stdout)

print("check_output")
print(subprocess.check_output("printenv", shell=True))

print("pipe")
subprocess.Popen("printenv", shell=True, stdout=sys.stdout, stderr=sys.stderr)
