#!/usr/bin/env python3
import sys
import logging
from env_helper import GPG_KEY

def main():
    logging.basicConfig(level=logging.INFO)

    print(f"hello ${GPG_KEY}")

if __name__ == "__main__":
    main()
