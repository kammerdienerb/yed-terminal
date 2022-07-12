#!/usr/bin/env bash

if [[ $(uname) == "Darwin" ]]; then
    WARN="-Wno-writable-strings -Wno-extern-c-compat"
else
    WARN="-Wno-write-strings -Wno-extern-c-compat"
    LINK="-lutil"
fi

g++ -o terminal.so terminal.cpp -std=c++11 ${WARN} ${LINK} $(yed --print-cppflags --print-ldflags)
