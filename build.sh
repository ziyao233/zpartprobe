#!/bin/sh

if [ $1x = releasex ]
then
	release="-O2"
else
	release="-g -O0"
fi

c99 zpartprobe.c -o zpartprobe -Wall -Werror $release -pedantic -Wextra
