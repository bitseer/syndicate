#!/usr/bin/python
#
# Interactive shell for testing SMDS
#
# $Id:
#

import os
import pydoc
import xmlrpclib

import sys
sys.path.append("/usr/share/SMDS")

from SMDS.mdapi import MDAPI
from SMDS.parameter import Mixed
from SMDS.auth import Auth
from SMDS.config import Config
from SMDS.method import Method
from SMDS.API.CDN.PL_CoBlitz import PL_CoBlitz
import SMDS.Methods
import traceback

import logger

class Callable:
   """
   Wrapper to call a method either directly or remotely and
   automagically add the authentication structure if necessary.
   """

   def __init__(self, shell, name, func, auth = None):
      self.shell = shell
      self.name = name
      self.func = func
      self.auth = auth

   def __call__(self, *args, **kwds):
      """
      Automagically add the authentication structure if the function
      requires it and it has not been specified.
      """

      if self.auth and \
         (not args or not isinstance(args[0], dict) or \
         (not args[0].has_key('AuthMethod') and \
            not args[0].has_key('session'))):
         args = (self.auth,) + args

      if self.shell.multi:
         self.shell.calls.append({'methodName': self.name, 'params': list(args)})
         return None
      else:
         try:
               return self.func(*args, **kwds)
         except Exception, fault:
               print "Exception FAULT:", fault
               raise
         except:
               print "UNKNOWN FAULT:", sys.exc_info()[0]
               raise


class Shell:
   def __init__(self,
               # Add API functions to global scope
               globals = None,
               # Configuration file
               config = None,
               # XML-RPC server
               url = None, xmlrpc = False, cacert = None,
               # API authentication method
               method = None,
               # Password authentication
               role = None, user = None, password = None,
               # Session authentication
               session = None):
      """
      Initialize a new shell instance. Re-initializes globals.
      """

      try:
         # If any XML-RPC options have been specified, do not try
         # connecting directly to the DB.
         if (url, method, user, password, role, cacert, xmlrpc) != \
                  (None, None, None, None, None, None, False):
               raise Exception

         # Otherwise, first try connecting directly to the DB. This
         # absolutely requires a configuration file; the API
         # instance looks for one in a default location if one is
         # not specified. If this fails, try connecting to the API
         # server via XML-RPC.
         self.api = MDAPI( config=config )
         self.config = self.api.config
         self.url = None
         self.server = None
      except Exception, err:
         # Try connecting to the API server via XML-RPC
         self.api = MDAPI()

         try:
               if config is None:
                  self.config = Config()
               else:
                  self.config = Config(config)
         except Exception, err:
               # Try to continue if no configuration file is available
               self.config = None

         if url is None:
               if self.config is None:
                  raise Exception, "Must specify API URL"

               path = "MDAPI"
               url = "https://" + self.config.MD_API_HOST + \
                  ":" + str(self.config.MD_API_PORTNUM) + \
                  "/" + path + "/"

         self.url = url
         self.server = xmlrpclib.ServerProxy(url, allow_none = 1)

      # Set up authentication structure

      # Default is to use session or capability authentication
      if (method, user, password) == (None, None, None):
         method = "capability"

      if method == "capability":
         # Load defaults from configuration file if using capability
         # authentication.
         if user is None and self.config is not None:
               user = self.config.MD_API_MAINTENANCE_USER
         if password is None and self.config is not None:
               password = self.config.MD_API_MAINTENANCE_PASSWORD
         if role is None:
               role = "admin"
      elif method is None:
         # Otherwise, default to password authentication
         method = "password"

      if role == "anonymous" or method == "anonymous":
         self.auth = {'AuthMethod': "anonymous"}
      else:
         if user is None:
               raise Exception, "Must specify username"

         if password is None:
               raise Exception, "Must specify password"

         self.auth = {'AuthMethod': method,
                        'Username': user,
                        'AuthString': password}

         if role is not None:
               self.auth['Role'] = role

      methods = SMDS.Methods.methods
      for method in methods:
         api_function = self.api.callable(method)

         if self.server is None:
               # Can just call it directly
               func = api_function
         else:
               func = getattr(self.server, method)

         # If the function requires an authentication structure as
         # its first argument, automagically add an auth struct to
         # the call.
         if api_function.accepts and \
            (isinstance(api_function.accepts[0], Auth) or \
               (isinstance(api_function.accepts[0], Mixed) and \
               filter(lambda param: isinstance(param, Auth), api_function.accepts[0]))):
               auth = self.auth
         else:
               auth = None

         callable = Callable(self, method, func, auth)

         # Add to ourself and the global environment. Add dummy
         # subattributes to support tab completion of methods with
         # dots in their names (e.g., system.listMethods).
         class Dummy: pass
         paths = method.split(".")

         if len(paths) > 1:
               first = paths.pop(0)

               if not hasattr(self, first):
                  obj = Dummy()
                  setattr(self, first, obj)
                  # Also add to global environment if specified
                  if globals is not None:
                     globals[first] = obj

               obj = getattr(self, first)

               for path in paths:
                  if not hasattr(obj, path):
                     if path == paths[-1]:
                           setattr(obj, path, callable)
                     else:
                           setattr(obj, path, Dummy())
                  obj = getattr(obj, path)
         else:
               setattr(self, method, callable)
               # Also add to global environment if specified
               if globals is not None:
                  globals[method] = callable

      # Override help(), begin(), and commit()
      if globals is not None:
         globals['help'] = self.help
         globals['begin'] = self.begin
         globals['commit'] = self.commit

      # Multicall support
      self.calls = []
      self.multi = False
      logger.info("Connected to %s" % (self.url))

   def help(self, topic = None):
      if isinstance(topic, Callable):
         pydoc.pager(self.system.methodHelp(topic.name))
      else:
         pydoc.help(topic)

   def begin(self):
      if self.calls:
         raise Exception, "multicall already in progress"

      self.multi = True

   def commit(self):
      if self.calls:
         ret = []
         self.multi = False
         results = self.system.multicall(self.calls)
         for result in results:
               if type(result) == type({}):
                  raise xmlrpclib.Fault(result['faultCode'], result['faultString'])
               elif type(result) == type([]):
                  ret.append(result[0])
               else:
                  raise ValueError, "unexpected type in multicall result"
      else:
         ret = None

      self.calls = []
      self.multi = False

      return ret
