#!/usr/bin/python

import os
import sys
import subprocess
import time
import socket
import urllib
import urllib2
import signal
import threading

SYNDICATE_ROOT = "/home/princeton_syndicate/syndicate"
YUM_PACKAGES = "subversion gcc gcc-c++ gnutls-devel openssl-devel boost-devel libxml2-devel libattr-devel curl-devel automake make libtool fcgi-devel texinfo fuse fuse-devel pyOpenSSL libgcrypt-devel python-uuid uriparser-devel wget openssh-clients protobuf protobuf-devel protobuf-compiler libssh2-devel squid"
HOSTNAME = socket.gethostname()
EXPERIMENT_SERVER_URL = "http://vcoblitz-cmi.cs.princeton.edu:40000/"
DATA_SERVER_URL = "http://vcoblitz-cmi.cs.princeton.edu/"

LOG_FD = None
VERBOSE = True

BOOTSTRAP_NODE_FILE = "/tmp/bootstrap-node"
CLIENT_OUT = "/tmp/syndicate-out.txt"
LAST_CMD = "/tmp/last-command"

NEW_DAEMON_URL = os.path.join( DATA_SERVER_URL, "tools/experiments/experimentd.py" )
COMMAND_URL = os.path.join( DATA_SERVER_URL, "command" )

HEARTBEAT_URL = os.path.join( DATA_SERVER_URL, "heartbeat" )

CLIENT_PROC = None
EXPERIMENT_PROC = None

PIDFILE="/tmp/experimentd.pid"

# log a message
def log( loglevel, msg ):
   txt = "[%s] %s: %s" % (time.ctime(), loglevel, msg)

   if LOG_FD != None:
      print >> LOG_FD, txt
      LOG_FD.flush()

   if VERBOSE:
      print txt


# post a message to the experiment server
def post_message( loglevel, msg_txt ):
   global HOSTNAME
   global EXPERIMENT_SERVER_URL
   global VERBOSE

   if VERBOSE:
      log( loglevel, msg_txt )

   values = { "host": HOSTNAME, "date": time.ctime(), "loglevel": loglevel, "msg": msg_txt }
   data = urllib.urlencode(values)

   req = urllib2.Request( EXPERIMENT_SERVER_URL, data )

   try:
      resp = urllib2.urlopen( req )
      resp.close()
   except Exception, e:
      log( "EXCEPTION", "post_message: %s" % e )


def sh(args):
   fd = open( "/tmp/last_cmd.out","w")
   rc = subprocess.call(args, stdout=fd, stderr=fd)
   fd.close()
   return rc

# bootstrap the node
def bootstrap_node():
   global YUM_PACKAGES
   global BOOTSTRAP_NODE_FILE

   # don't bother if we already set everything up
   if os.path.exists( BOOTSTRAP_NODE_FILE ):
      try:
         bootstrap_fd = open( BOOTSTRAP_NODE_FILE, "r" )
         bootstrap_date = bootstrap_fd.read()
         bootstrap_fd.close()

         log( "INFO", "Node bootstrapped on %s" % bootstrap_date )
         return 0
      except Exception, e:
         log( "WARN", "Could not read %s (exception = %s), will re-bootstrap node" % (BOOTSTRAP_NODE_FILE, e ))
         os.unlink( BOOTSTRAP_NODE_FILE )

   # what FC version are we?
   fd = open("/etc/redhat-release", "r")
   rel = fd.read()
   fd.close()

   rel = rel.strip()
   post_message( "INFO", "redhat release: '%s'" % rel)
   
   # update
   rc = sh(["yum", "-y", "update"])
   if rc != 0:
      post_message( "ERROR", "bootstrap_node: 'yum update' rc = %s" % rc )
      return rc

   # install packages
   rc = sh(["yum", "-y", "install"] + YUM_PACKAGES.split())
   if rc != 0:
      post_message( "ERROR", "bootstrap_node: 'yum install' rc = %s" % rc )
      return rc

   post_message( "INFO", "successfully bootstrapped node" )

   date_ctime = time.ctime()
   bootstrap_fd = open( BOOTSTRAP_NODE_FILE, "w" )
   bootstrap_fd.write( date_ctime )
   bootstrap_fd.close()

   return 0


def make( target ):
   global SYNDICATE_ROOT
   rc = sh(["make", "-C", SYNDICATE_ROOT, target])
   if rc != 0:
      post_message( "ERROR", "bootstrap_client: 'make %s' rc = %s" % (target, rc) )
   return rc


# bootstrap a Syndicate client
def bootstrap_client():
   global SYNDICATE_ROOT
   global BOOTSTRAP_NODE_FILE

   if not os.path.exists( BOOTSTRAP_NODE_FILE ):
      rc = bootstrap_node()
      if rc != 0:
         return rc

   # install Syndicate
   if os.path.exists( SYNDICATE_ROOT ):
      rc = sh(["svn", "up", SYNDICATE_ROOT])
      if rc != 0:
         post_message( "ERROR", "bootstrap_client: build clean rc = %s" % rc )
         return rc

   else:
      rc = sh(["svn", "checkout", "https://svn.princeton.edu/cdnfs", SYNDICATE_ROOT])
      if rc != 0:
         post_message( "ERROR", "bootstrap_client: 'svn checkout' rc = %s" % rc )
         return rc

   rc = make( "libmicrohttpd-install" )
   if rc != 0:
      return rc

   rc = make( "common-install" )
   if rc != 0:
      return rc

   rc = make( "client-install" )
   if rc != 0:
      return rc
      
   post_message( "INFO", "successfully bootstrapped client" )
   return 0


# run the client, but catch any errors and report them
def start_client():
   global CLIENT_OUT

   out_fd = None
   try:
      out_fd = open( CLIENT_OUT, "w" )
   except Exception, e:
      post_message( "ERROR", "failed to start client: %s" % e )
      return None

   client_proc = None

   try:
      client_proc = subprocess.Popen( ["syndicate-httpd", "-f"], stdout=out_fd, stderr=out_fd )
   except:
      pass

   if client_proc != None:
      post_message( "INFO", "successfully started client" )

   return client_proc


# wait for the client to die
def wait_client():
   rc = client_proc.wait()
   post_message( "INFO", "client exit code %s" % rc )



# kill all processes with the given substring in their name
def killall( name, sig, ignorePids=None ):
   ps = subprocess.Popen("ps aux | grep %s | grep -v grep | awk '{print $2}'" % name, shell=True, stdout=subprocess.PIPE)
   data = ps.stdout.read()
   ps.wait()

   if data:
      pidstrs = data.split()

      for pidstr in pidstrs:
         pid = int(pidstr)
         if ignorePids != None:
            if pid in ignorePids:
               continue

         os.kill( pid, sig )



# stop all clients
def stop_all_clients():
   killall( "syndicate-", signal.SIGTERM )
   killall( "syndicate-", signal.SIGKILL )
   post_message( "INFO", "Killed all clients" )


# stop a subprocess
def stop_subprocess( proc, name ):
   if proc == None:
      return
      
   # try SIGINT, then SIGKILL
   try:
      rc = proc.poll()
      if rc != None:
         post_message( "INFO", "%s exited, rc = %s" % (name, rc))
         return
   except:
      post_message( "INFO", "%s presumed dead" % (name) )
      return

      
   # sigint it
   try:
      proc.send_signal( signal.SIGINT )
   except:
      post_message( "INFO", "%s signal failure; presumed dead" % (name) )
      return

   attempts = 5
   while attempts > 0:

      try:
         rc = proc.poll()
         if rc != None:
            break
      except:
         break

      time.sleep(1)
      attempts -= 1

   if rc == None:
      # client isn't dead yet
      try:
         proc.kill()
         post_message( "WARN", "%s had to be killed" % name )
      except:
         post_message( "WARN", "kill %s failed; presumed dead" % (name) )
         return

   post_message( "INFO", "%s exit code %s" % (name, rc) )
   return 


# stop a client
def stop_client( client_proc ):
   return stop_subprocess( client_proc, "client" )
   
# stop an experiment
def stop_experiment( experiment_proc ):
   return stop_subprocess( experiment_proc, "experiment" )
   
   
# run an experiment
def run_experiment( experiment_url ):
   global EXPERIMENT_PROC
   
   # first, download the experiment
   experiment_name = os.path.basename(experiment_url)
   experiment_path = "/tmp/%s" % experiment_name
   experiment_fd = open(experiment_path, "w")
   
   rc = subprocess.call(["curl", "-ks", experiment_url], stdout=experiment_fd)
   experiment_fd.close()
   if rc != 0:
      post_message("ERROR", "could not download experiment %s, rc = %s" % (experiment_url, rc))
      return rc

   try:
      os.chmod( experiment_path, 0755 )
   except:
      post_message("ERROR", "could not chmod %s, rc = %s" % (experiment_path, rc))
      return None

   # run the experiment!
   experiment_data = None
   try:
      experiment_log = open(experiment_path + ".log", "w")
      EXPERIMENT_PROC = subprocess.Popen( [experiment_path], stdout=experiment_log, stderr=experiment_log )
      rc = EXPERIMENT_PROC.wait()
      experiment_log.close()
   except Exception, e:
      experiment_data = str(e)

   EXPERIMENT_PROC = None
   if experiment_data == None:
      experiment_log = open(experiment_path + ".log", "r")
      experiment_data = experiment_log.read()
      experiment_log.close()

   post_message("INFO", "\n---------- BEGIN %s ----------\n%s\n---------- END %s ----------\n" % (experiment_name, experiment_data, experiment_name) )
   return rc


# upgrade ourselves
def upgrade():
   global NEW_DAEMON_URL
   global CLIENT_PROC
   
   fd = open("/tmp/experimentd.py", "w")
   rc = subprocess.call(["curl", "-ks", NEW_DAEMON_URL], stdout=fd)
   fd.close()
   if rc != 0:
      return rc
   else:
      pid = os.fork()
      if pid == 0:
         shutdown()
         os.setsid()
         os.chmod("/tmp/experimentd.py", 0755)
         os.execv("/tmp/experimentd.py", ["/tmp/experimentd.py", "-u"])
      else:
         time.sleep(1)
         cleanup(None, None)
         

# get the next command from the server
def get_command():
   global COMMAND_URL

   req = urllib2.Request( COMMAND_URL )

   data = None
   lastmod = None

   try:
      resp = urllib2.urlopen( req )
      data = resp.read()
      info = resp.info()

      if info.has_key("Last-Modified"):
         lastmod = info["Last-Modified"]
      else:
         post_message( "ERROR", "Failed to get info from %s" % COMMAND_URL )
         data = None

      resp.close()
   except:
      post_message( "ERROR", "Failed to read %s" % COMMAND_URL )
      return None

   if data != None and lastmod != None:
      return "%s %s" % (data, lastmod)
   else:
      return None


# determine if a command is fresh
def is_command_fresh( command ):
   global LAST_CMD

   data = None

   if os.path.exists( LAST_CMD ):
      try:
         fd = open( LAST_CMD, "r" )
         data = fd.read()
         fd.close()
      except:
         os.unlink( LAST_CMD )
         return True

      if data == command:
         return False

      else:
         return True

   else:
      return True


# store a received command
def store_command( command ):
   global LAST_CMD

   try:
      fd = open( LAST_CMD, "w" )
      fd.write( command )
      fd.close()
      return 0
   except:
      return -1


# prepare for shutdown
def shutdown():
   global CLIENT_PROC
   global EXPERIMENT_PROC
   stop_experiment( EXPERIMENT_PROC )
   stop_client( CLIENT_PROC )
   stop_all_clients()

   CLIENT_PROC = None
   EXPERIMENT_PROC = None
   
   LOG_FD.close()
   

# signal handler
def cleanup( signal, frame ):
   shutdown()
   sys.exit(0)


# process a command
def process_command( command ):
   global CLIENT_PROC
   global LAST_CMD
   global NEW_DAEMON_URL

   ok = True

   # format: COMMAND [args] LASTMOD
   command_name = command.split()[0]

   if command_name == "bootstrap":
      post_message( "INFO", "bootstrapping client" )
      rc = bootstrap_client()
      post_message( "INFO", "bootstrap client rc = %s" % rc )

      
   elif command_name == "start":
      post_message( "INFO", "starting client" )
      client_proc = start_client()
      if client_proc != None:
         post_message( "INFO", "client started" )
         CLIENT_PROC = client_proc
      else:
         post_message( "ERROR", "client failed to start" )

         
   elif command_name == "stop":
      post_message( "INFO", "stopping client" )
      stop_client( CLIENT_PROC )
      CLIENT_PROC = None
      stop_all_clients()
      post_message( "INFO", "client stopped" )

      
   elif command_name == "restart":
      post_message( "INFO", "restarting client" )

      if CLIENT_PROC:
         stop_client( CLIENT_PROC )

      stop_all_clients()
      client_proc = start_client()

      if client_proc != None:
         post_message("INFO", "restarted client" )
         CLIENT_PROC = client_proc

      else:
         post_message("ERROR", "failed to restart client")


   elif command_name == "reload":
      post_message( "INFO", "reloading client" )

      if CLIENT_PROC:
         stop_client( CLIENT_PROC )

      stop_all_clients()
      rc = bootstrap_client()
      if rc != 0:
         post_message("ERROR", "failed to re-bootstrap client, rc = %s" % rc )
      else:
         client_proc = start_client()

         if client_proc != None:
            post_message( "INFO", "reloaded client" )
            CLIENT_PROC = client_proc
         else:
            post_message( "ERROR", "failed to reload client" )


   elif command_name == "exit":
      post_message( "INFO", "shutting down" )
      stop_all_clients()
      unlink( LAST_CMD )
      sys.exit(0)

   elif command_name == "upgrade":
      if CLIENT_PROC != None:
         post_message( "ERROR", "client is running")

      else:
         # need to store the command NOW
         rc = store_command( command )
         if rc != 0:
            post_message( "ERROR", "store_command rc = %s" % rc)

         else:
            rc = upgrade()
            if rc != 0:
               post_message("ERROR", "upgrade rc = %s" % rc)


   elif command_name == "experiment":
      # verify that the client is running
      run = True
      if CLIENT_PROC != None:
         rc = None
         try:
            rc = CLIENT_PROC.poll()
         except:
            pass
         
         if rc != None:
            # client died
            post_message( "ERROR", "client died before experiment could run (rc = %s)" % rc)
            run = False

      if run:
         experiment_url = command.split()[1]
         rc = run_experiment( experiment_url )

         post_message( "INFO", "experiment %s rc = %s" % (os.path.basename(experiment_url), rc) )

         # verify that the client still lives
         if CLIENT_PROC != None:
            rc = None
            try:
               rc = CLIENT_PROC.poll()
            except:
               pass
            
            if rc != None:
               # client died during the experiment
               post_message( "ERROR", "client died during experiment (rc = %s)" % rc)
            
         
   elif command_name == "nothing":
      ok = True

   else:
      post_message("ERROR", "Unknown command '%s'" % command_name)
      ok = False

   return ok


# check the heartbeat status every so often
class HBThread( threading.Thread ):
   def run(self):
      global HEARTBEAT_URL
      global EXPERIMENT_PROC
      global CLIENT_PROC
      while True:
         req = urllib2.Request( HEARTBEAT_URL )
         dat = None
         try:
            resp = urllib2.urlopen( req )
            dat = resp.read()
            resp.close
         except Exception, e:
            log( "EXCEPTION", "Failed to read %s, exception = %s" % (HEARTBEAT_URL, e) )

         if dat != None:
            dat = dat.strip()
            if dat == "force-stop":
               # kill everything
               stop_experiment( EXPERIMENT_PROC )
               stop_client( CLIENT_PROC )
               stop_all_clients()

               CLIENT_PROC = None
               EXPERIMENT_PROC = None

         time.sleep(60)



if __name__ == "__main__":
   if os.path.exists(PIDFILE):
      pidf = open(PIDFILE,"r")
      pid = int( pidf.read().strip() )
      try:
         os.kill( pid, signal.SIGTERM )
         time.sleep(1.0)
         os.kill( pid, signal.SIGKILL )
      except:
         pass

      os.unlink( PIDFILE )

   pid = os.fork()
   if pid > 0:
      sys.exit(0)

   os.setsid()

   mypid = os.getpid()
   fd = open(PIDFILE, "w" )
   fd.write( str(mypid) )
   fd.close()

   # heartbeat
   hb_thread = HBThread()
   hb_thread.start()
  
   killall("experiment-heartbeat", signal.SIGKILL)
    
   LOG_FD = open("/tmp/experimentd.log", "w" )
   HOSTNAME = socket.gethostname()

   signal.signal( signal.SIGINT, cleanup )
   signal.signal( signal.SIGQUIT, cleanup )
   signal.signal( signal.SIGTERM, cleanup )
   
   if '-u' in sys.argv:
      post_message( "INFO", "upgraded" )

   while True:
      cmd = get_command()
      if cmd == None:
         log( "ERROR", "Could not get command" )
      else:
         if is_command_fresh( cmd ):
            rc = process_command( cmd )
            if not rc:
               log("ERROR", "Failed to process command '%s'" % cmd )
            else:
               rc = store_command( cmd )
               if rc != 0:
                  log("ERROR", "Failed to store command")



      time.sleep(60)

