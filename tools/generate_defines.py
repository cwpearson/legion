#!/usr/bin/env python

# Copyright 2019 Stanford University
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

from __future__ import print_function

import argparse
import sys
try:
    from shlex import quote
except ImportError:
    import re
    def quote(s):
        if (s == '') or re.search(' \t\'\"', s):
            # wrap in single quotes and escape any quotes inside
            return "'" + s.replace("'", "'\\''") + "'"
        else:
            return s

def make_define(arg):
    values = arg.split('=', 1)
    if len(values) == 1:
        return '''
#ifndef {}
#define {}
#endif
'''.format(values[0], values[0])
    else:
        return '''
#ifndef {}
#define {} {}
#endif
'''.format(values[0], values[0], values[1])

def generate_defines():
    parser = argparse.ArgumentParser()
    parser.add_argument('-D', dest='defines', action='append', default=[])
    parser.add_argument('-o', '--output',
                        help='name of output file to write')
    parser.add_argument('-c', '--check-unchanged', action='store_true',
                        help='avoid touching file if contents would not change')
    parser.add_argument('-v', '--verbose', action='store_true')
    args = parser.parse_args()

    defines = args.defines

    contents = '/* Generated by: {} */\n'.format(' '.join(map(quote, sys.argv)))
    for define in defines:
        contents += make_define(define)

    if args.output:
        if args.check_unchanged:
            try:
                oldcontents = open(args.output, 'r').read()
                if contents == oldcontents:
                    if args.verbose:
                        print('Contents of \'{}\' have not changed.'.format(args.output))
                    exit(0)
                else:
                    if args.verbose:
                        print('Contents of \'{}\' have changed - rewriting file.'.format(args.output))
            except IOError:
                # file does not exist or not readable - either way, we want to
                #  fall through and try to write it ourselves
                pass
        with open(args.output, 'w') as f:
            f.write(contents)
    else:
        sys.stdout.write(contents)

if __name__ == '__main__':
    generate_defines()
