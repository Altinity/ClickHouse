#!/usr/bin/python3

import sys

if __name__ == '__main__':
    for chunk_header in sys.stdin:
        chunk_length = int(chunk_header)
        print(1, end='\n')
        print(str(chunk_length), end='\n')

        while chunk_length != 0:
            line = sys.stdin.readline()
            chunk_length -= 1

        sys.stdout.flush()
