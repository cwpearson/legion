#!/usr/bin/env python

# parses a GitLab CI YAML configuration file and generates a Dockerfile for
#  a specified job

import yaml
import argparse
import os
import tempfile
import subprocess

parser = argparse.ArgumentParser(description='Generate Dockerfile from GitLab CI YAML configuration file')
parser.add_argument('-b', '--branch', type=str, default='master',
                    help='branch of repository to test')
parser.add_argument('--repo', type=str,
                    default='https://gitlab.com/StanfordLegion/legion.git',
                    help='repository to clone')
parser.add_argument('-l', '--localtree', type=str,
                    help='local tree to test (instead of cloning)')
parser.add_argument('-o', '--outdir', type=str,
                    help='write Dockerfile to output directory instead of building')
parser.add_argument('-k', '--keep', action='store_true',
                    help='modify test.py invocation to keep results')
parser.add_argument('-i', '--image', type=str,
                    help='override base container image')
parser.add_argument('-n', '--noscript', action='store_true',
                    help='do not actually run /script.sh during docker build')
parser.add_argument('-t', '--tag', type=str,
                    help='tag to apply to built container')
parser.add_argument('cfgfile', type=str,
                    help='path to GitLab CI config file')
parser.add_argument('jobname', type=str,
                    help='name of job to generate Dockerfile for')
args = parser.parse_args()

try:
    f = open(args.cfgfile, 'r')
    cfg = yaml.safe_load(f)
except:
    print('ERROR: could not read configuration file: {}'.format(args.cfgfile))
    raise

if args.jobname not in cfg:
    print('ERROR: job \'{}\' not found in configuration file'.format(args.jobname))
    exit(1)

def generate_script(args, cfg, job):
    s = ''
    if 'before_script' in cfg:
        for cmd in cfg['before_script']:
            s += cmd.replace('\\n','\n')
            if not cmd.endswith('\n'):
                s += '\n'

    for cmd in job['script']:
        if args.keep:
            cmd = cmd.replace('test.py', 'test.py --keep || /bin/true')
        s += cmd.replace('\\n','\n')
        if not cmd.endswith('\n'):
            s += '\n'

    return s

def generate_dockerfile(args, cfg, job, script=None):
    s = ''
    s += 'FROM {}\n'.format(args.image or job['image'])
    s += 'SHELL [ "/bin/bash", "-c" ]\n'
    for k, v in cfg['variables'].iteritems():
        s += 'ENV {}="{}"\n'.format(k, v)
    for k, v in job.get('variables', {}).iteritems():
        s += 'ENV {}="{}"\n'.format(k, v)

    if script:
        b64 = script.encode('base64')[0:-1]  # trim trailing newline
        b64 = b64.replace('\n', '\\\n')
        s += 'RUN echo \\\n' + b64 + ' | base64 -d > script.sh\n'
    else:
        s += 'COPY script.sh .\n'
    s += 'RUN chmod a+x ./script.sh\n'

    if args.localtree:
        reclone = False
        # does it look like a git repo?  if so, just copy in .git and clone
        #  from that
        if os.path.isdir(os.path.join(args.localtree, '.git')):
            # check to see if HEAD is up to date with tree
            ret = subprocess.call([ 'git', '-C', args.localtree, 'diff-index', '--quiet', 'HEAD'])
            if ret == 0:
                reclone = True
            else:
                print('WARNING: local tree looks like a git repository, but has uncommitted changes - copying entire tree to be safe')

        if reclone:
            args.localtree = os.path.join(args.localtree, '.git')
            s += 'COPY / localtree.git\n'
            s += 'RUN git clone localtree.git repo\n'
        else:
            # no?  just copy the whole thing in and hope for the best
            s += 'COPY / repo\n'
    else:
        s += 'RUN git clone -b {} {} repo\n'.format(args.branch, args.repo)
    s += 'WORKDIR "/repo"\n'

    if not args.noscript:
        s += 'RUN /script.sh\n'

    return s

job = cfg[args.jobname]

if args.outdir:
    # no support for --tag or --localtree
    if args.tag or args.localtree:
        print('FATAL: --outdir cannot be used with --localtree and/or --tag!')
        exit(1)

    dfilename = os.path.join(args.outdir, 'Dockerfile')
    sfilename = os.path.join(args.outdir, 'script.sh')
    
    with open(dfilename, 'w') as f:
        f.write(generate_dockerfile(args, cfg, job))
    with open(sfilename, 'w') as f:
        f.write(generate_script(args, cfg, job))
else:
    # generate combined dockerfile and script that we can pipe directly into
    #  a 'docker build' command
    scr = generate_script(args, cfg, job)
    df = generate_dockerfile(args, cfg, job, script=scr)

    cmd = [ 'docker', 'build' ]

    if args.tag:
        cmd.extend([ '-t', args.tag ])

    if args.localtree:
        # we use the target tree as the context, which means we need to
        #  put the dockerfile somewhere else (and tell docker about it)
        fd, dfilename = tempfile.mkstemp(text=True)
        os.write(fd, df)
        os.close(fd)
        cmd.extend([ '-f', dfilename, os.path.abspath(args.localtree) ])
    else:
        cmd.append('-')

    p = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    p.communicate(df)
    ret = p.wait()
    if args.localtree:
        # clean up temp file
        os.unlink(dfilename)
    exit(ret)
