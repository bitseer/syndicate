#!/usr/bin/python

""" Syndicate configurator.  """

import httplib
import os
import shutil
import tools

from threading import Thread
import logger
import tools
import subprocess

# local test or production run?
TEST_ONLY=False

# Syndicate package dependencies
YUM_DEPS=[
   "fuse",
   "libcurl",
   "libxml2",
   "gnutls",
   "libgcrypt",
   "openssl",
   "boost",
   "libattr",
   "uriparser",
   "protobuf"
]

# Syndicate information repository
SYNDICATE_REPOSITORY="vcoblitz-cmi.cs.princeton.edu"
SYNDICATE_RPM="/syndicate/syndicate-UG.rpm"


def start():
   logger.log('syndicate plugin starting up...')


def container_command_args( slice_name, args ):
   ret = ["vserver", "%s" % slice_name, "suexec", "0"] + args
   return ret
   
def container_subprocess_call( slice_name, args, stdin=None, stdout=None, stderr=None, shell=False ):
   """
   Run a process inside a container.
   """
   cmd = container_command_args( slice_name, args )
   rc = subprocess.call( cmd, stdin=stdin, stdout=stdout, stderr=stderr, shell=shell )
   return rc
   

def syndicate_op(op, mountpoint, syndicate_ip):
   global TEST_ONLY
   
   if TEST_ONLY:
      result = None
      return result

   logger.log("Syndicate: Http op %s on url %s to host %s" % (op, mountpoint, syndicate_ip))

   try:
      conn = httplib.HTTPSConnection(syndicate_ip, timeout=60)
      conn.request(op, mountpoint)
      r1 = conn.getresponse()
   except:
      logger.log_exc("Exception when contacting syndicate sliver", "Syndicate")

   if (r1.status / 100) != 2:
      logger.log("Syndicate: Error: Got http result %d on %s" % (r1.status, mountpoint))
      return False

   return result


def enable_syndicate_mount(sliver, mountpoint, syndicate_ip):
   if not os.path.exists(mountpoint):
      try:
         os.mkdir(mountpoint)
      except:
         logger.log_exc("failed to mkdir syndicate mountpoint", "Syndicate")
         return

   syndicate_op("PUT", mountpoint, syndicate_ip)
   

def disable_syndicate_mount(sliver, mountpoint, syndicate_ip):
   syndicate_op("DELETE", mountpoint, syndicate_ip)

   if os.path.exists(mountpoint):
      try:
         os.rmdir(mountpoint)
      except:
         logger.log_exc("failed to delete syndicate mountpoint", "Syndicate")



def bootstrap_syndicate(sliver, syndicate_ip):
   """
   Bootstrap the Syndicate UG on this sliver.
   """
   global YUM_DEPS
   
   # first, make sure the required dependencies are installed
   rc = container_subprocess_call( sliver['name'], ["yum", "-y", "install"] + YUM_DEPS )
   if rc != 0:
      logger.log( "Syndicate: failed to install dependencies, rc = %s" % rc )
      return False

   # next, get the Syndicate RPM and install it
   
   
   

def GetSlivers(data, conf = None, plc = None):
   node_id = tools.node_id()

   if 'slivers' not in data:
      logger.log_missing_data("syndicate.GetSlivers",'slivers')
      return

   syndicate_sliver = None
   for sliver in data['slivers']:
      if sliver['name'] == "princeton_syndicate":
         syndicate_sliver = sliver

   if not syndicate_sliver:
      logger.log("Syndicate: no princeton_syndicate sliver on this node. aborting.")
      return

   syndicate_ip = tools.get_sliver_ip("princeton_syndicate")
   if not syndicate_ip:
      logger.log("Syndicate: unable to get syndicate sliver ip. aborting.")
      return

   for sliver in data['slivers']:
      enable_syndicate = False

      # build a dict of attributes, because it's more convenient
      attributes={}
      for attribute in sliver['attributes']:
         attributes[attribute['tagname']] = attribute['value']

      sliver_name = sliver['name']
      syndicate_mountpoint = os.path.join("/vservers", sliver_name, "syndicate")
      enable_syndicate = attributes.get("enable_syndicate", False)
      has_syndicate = os.path.exists(syndicate_mountpoint)

      if enable_syndicate and (not has_syndicate):
         logger.log("Syndicate: enabling syndicate for %s" % sliver_name)
         #enable_syndicate_mount(sliver, syndicate_mountpoint, syndicate_ip)
         t = Thread(target=enable_syndicate_mount, args=(sliver, syndicate_mountpoint, syndicate_ip))
         t.start()

      elif (not enable_syndicate) and (has_syndicate):
         logger.log("Syndicate: disabling syndicate for %s" % sliver_name)
         #disable_syndicate_mount(sliver, syndicate_mountpoint, syndicate_ip)
         t = Thread(target=disable_syndicate_mount, args=(sliver, syndicate_mountpoint, syndicate_ip))
         t.start()

