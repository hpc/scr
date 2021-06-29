#! /usr/bin/env python3

# scr_watchdog.py

# This is a generic utility for detecting when an application that uses
# SCR is hung. It periodically checks the flush file to see if checkpointing
# activity has occurred since the last time it checked. If too much time
# passes without activity, it kills the job

import argparse, time
#from datetime import datetime
from pyfe import scr_const
from pyfe.scr_param import SCR_Param
from pyfe.resmgr.scr_resourcemgr import SCR_Resourcemgr
#from pyfe.scr_kill_jobstep import scr_kill_jobstep
from pyfe.scr_common import runproc

def scr_watchdog(prefix=None,jobstepid=None,scr_env=None):
  # check that we have a  dir and apid
  if prefix is None or jobstepid is None:
    return 1

  param = None
  resmgr = None
  if scr_env is not None:
    param = scr_env.param
    resmgr = scr_env.resmgr

  # lookup timeout values from environment
  if param is None:
    param = SCR_Param()
  if resmgr is None:
    resmgr = SCR_Resourcemgr()
  timeout = None
  timeout_pfs = None

  # we have two timeout variables now, one for the length of time to wait under
  # "normal" circumstances and one for the length of time to wait if writing
  # to the parallel file system
  param_timeout = param.get('SCR_WATCHDOG_TIMEOUT')
  if param_timeout is not None:
    timeout = param_timeout

  param_timeout_pfs = param.get('SCR_WATCHDOG_TIMEOUT_PFS')
  if param_timeout_pfs is not None:
    timeout_pfs = param_timeout_pfs

  # TODO: What to do if timeouts are not set? die? should we set default values?
  # for now die with error message

  # start_time = datetime.now() ## this is not used?

  if timeout is None or timeout_pfs is None:
    print('Necessary environment variables not set: SCR_HANG_TIMEOUT and SCR_HANG_TIMEOUT_PFS')
    return 1

  # loop periodically checking the flush file for activity
  lastCheckpoint    = ''
  lastCheckpointLoc = ''

  getLatestCmd    = 'scr_flush_file --dir '+prefix+' -l'
  getLatestLocCmd = 'scr_flush_file --dir '+prefix+' -L'

  timeToSleep = int(timeout)

  while True:
    time.sleep(timeToSleep)
    #print "was sleeping, now awake\n";
    argv = getLatestCmd.split(' ')
    latest = runproc(argv=argv,getstdout=True)[0]
    #print "latest was $latest\n";
    latestLoc = ''
    if latest!='':
      argv = getLatestLocCmd.split(' ')
      argv.extend(latest.split(' ')[0])
      latestLoc = runproc(argv=argv,getstdout=True)[0]
    #print "latestLoc was $latestLoc\n";
    if latest == lastCheckpoint:
      if latestLoc == lastCheckpointLoc:
        #print "time to kill\n";
        break
    lastCheckpoint = latest
    lastCheckpointLoc = latestLoc
    if latestLoc == 'SYNC_FLUSHING':
      timeToSleep = int(timeout_pfs)
    else:
      timeToSleep = int(timeout)

  print('Killing simulation using scr_kill_jobstep --jobStepId '+jobstepid)
  resmgr.scr_kill_jobstep(jobid=jobstepid)
  return 0

if __name__=='__main__':
  parser = argparse.ArgumentParser(add_help=False, argument_default=argparse.SUPPRESS, prog='scr_watchdog')
  parser.add_argument('-h','--help', action='store_true', help='Show this help message and exit.')
  parser.add_argument('-d','--dir', metavar='<prefixDir>', type=str, default=None, help='Specify the prefix directory.')
  parser.add_argument('-j','--jobStepId', metavar='<jobStepId>', type=str, default=None, help='Specify the jobstep id.')
  args = vars(parser.parse_args())
  if 'help' in args:
    parser.print_help()
  elif args['dir'] is None or args['jobStepId'] is None:
    print('Prefix directory and job step id must be specified.')
  else:
    ret = scr_watchdog(prefix=args['prefix'],jobstepid=args['jobStepId'])
    print('scr_watchdog returned '+str(ret))