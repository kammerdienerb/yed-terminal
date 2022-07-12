#!/usr/bin/env bash
g++ -o terminal.so terminal.cpp -std=c++11 -Wno-writable-strings -Wno-extern-c-compat $(yed --print-cppflags --print-ldflags)
