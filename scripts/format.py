#!/usr/bin/env python3
import re
import os
import subprocess
from argparse import ArgumentParser

if __name__ == "__main__":
    parser = ArgumentParser()
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()

    regex = re.compile('(.*\.c$)|(.*\.cpp$)|(.*\.h$)|(.*\.hpp$)')
    format_ok = True

    for path in ['src']:
        for root, dirs, files in os.walk(path):
            for file in files:
                print(file)
                if regex.match(file):
                    file_path = os.path.join(root, file)
                    if args.check:
                        cmd_str = ['clang-format', '--dry-run', '--Werror',
                                '-style', 'WebKit', f'{file_path}']
                    else:
                        cmd_str = ['clang-format', '-i', '-style', 'WebKit', f'{file_path}']
                    try:
                        subprocess.run(cmd_str, capture_output=False, check=True)
                    except Exception as e:
                        format_ok = False
    if not format_ok:
        exit(1)
