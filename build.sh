#!/bin/sh
# WRSR Mail - a postal service for Workers & Resources: Soviet Republic
# Copyright (C) 2026 TheRealMeowMeow00
# SPDX-License-Identifier: GPL-3.0-only
# Build mail.dll and run the offline test. Installs nothing.
set -e
T="${MAIL_TOOLCHAIN:-/c/Projects/toolchain/llvm-mingw-20260922-ucrt-x86_64/bin}"
EXE="${MAIL_GAME_EXE:-G:/SteamLibrary/steamapps/common/SovietRepublic/SOVIET64.exe}"
cd "$(dirname "$0")"
mkdir -p out
"$T"/x86_64-w64-mingw32-clang++ -O2 -std=c++17 -shared -static -Wall -Wno-unused-function \
    -o out/mail.dll plugins/mail/mail.cpp
cp plugins/mail/mail.ini out/mail.ini
mkdir -p item/plugins
cp out/mail.dll out/mail.ini item/plugins/
python tools/make_icons.py
"$T"/x86_64-w64-mingw32-clang++ -O1 -std=c++17 -static -Wno-unused-function \
    -o out/test_observer.exe test/test_observer.cpp
./out/test_observer.exe "$EXE"
"$T"/x86_64-w64-mingw32-clang++ -O1 -std=c++17 -static -Wno-unused-function -Wno-invalid-offsetof \
    -o out/sim_chain.exe test/sim_chain.cpp
$T/llvm-objdump -p out/mail.dll | grep -E 'TsmPlugin|DLL Name'
