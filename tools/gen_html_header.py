#!/usr/bin/env python3
"""Convert an HTML file to a C string literal header.

Usage: gen_html_header.py <input.html> > output.h
"""
import sys


def main():
    if len(sys.argv) != 2:
        print("Usage: gen_html_header.py <input.html>", file=sys.stderr)
        sys.exit(1)

    src = sys.argv[1]
    with open(src, 'r', encoding='utf-8') as f:
        content = f.read()

    print("/* Auto-generated from {} — do not edit; run 'make' to regenerate. */".format(src))
    print("static const char HTML_PAGE[] =")
    for line in content.splitlines():
        escaped = line.replace('\\', '\\\\').replace('"', '\\"')
        print('"{line}\\n"'.format(line=escaped))
    print(";")


if __name__ == '__main__':
    main()
