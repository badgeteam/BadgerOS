#!/usr/bin/env bash

./kernel/tools/sysgen.py
cp kernel/misc/syscall.h mlibc/sysdeps/badgeros/include/sys/syscall.h
cp kernel/misc/syscall.c mlibc/sysdeps/badgeros/generic/syscall.cpp
