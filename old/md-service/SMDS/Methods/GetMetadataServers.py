#!/usr/bin/python

from SMDS.method import Method
from SMDS.mdserver import *
from SMDS.user import *
from SMDS.parameter import *
from SMDS.auth import Auth
from SMDS.filter import *
from SMDS.faults import *

class GetMetadataServers( Method ):
   """
   Get a list of zero or more extant metadata servers.
   """
   
   accepts = [
         Auth(),
         Filter( MDServer.fields ),
         Parameter([str], "List of fields to return", nullok = True )
   ]
   roles = ['admin','user']
   returns = [MDServer.fields]
   
   def call(self, auth, mdserver_fields, return_fields ):
      
      assert self.caller is not None
      
      roles = self.caller['roles']
      
      users = Users( self.api, {'username': auth['Username']} )
      user = users[0]
      
      if not return_fields:
         return_fields = [name for (name,param) in MDServer.fields.items()]
         
      mdservers = MDServers( self.api, mdserver_fields )
      
      ret = []
      for md in mdservers:
         md_dict = {}
         for rf in return_fields:
            if rf in md:
               md_dict[rf] = md[rf]
         ret.append( md_dict )
      
      return ret