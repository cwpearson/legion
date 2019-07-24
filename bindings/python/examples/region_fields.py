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

import legion
from legion import task, RW, R, WD, N
import numpy

@task(privileges=[RW('x')])
def init_x(R):
    R.x.fill(123)

@task(privileges=[RW('y')])
def init_y(R):
    R.y.fill(456)

@task(privileges=[RW('x')])
def inc(R, step):
    numpy.add(R.x, step, out=R.x)

@task(privileges=[R('x', 'y')])
def check(R):
    assert numpy.all(R.x == 2035)
    assert numpy.all(R.y == 456)
    print('Test passed')

@task(privileges=[RW('x') + R('y')])
def saxpy(R, a):
    numpy.add(R.x, a * R.y, out=R.x)

@task
def main():
    R = legion.Region.create([4, 4], {'x': legion.float64, 'y': legion.float64})
    legion.fill(R, 'x', 101)
    legion.fill(R, 'y', 102)
    init_x(R)
    init_y(R)
    inc(R, 1000)
    saxpy(R, 2)
    check(R)

if __name__ == '__main__':
    main()
