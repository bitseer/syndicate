#!/usr/bin/python

from SMDS.method import Method
from SMDS.user import *
from SMDS.parameter import *
import SMDS.auth
from SMDS.auth import Auth
from SMDS.faults import *

class AddUser( Method ):
   """
   Add a user account.  The account will be disabled at first.
   """
   
   accepts = [
         Auth(),
         dict([(n,User.fields[n]) for n in ['username','password','email']])
   ]
   roles = ['admin']
   returns = Parameter(int, "The user's UID (positive number) if successful")
   
   def call(self, auth, user_fields):
      assert self.caller is not None
      
      # hash the password before we store it
      m = SMDS.auth.new_sha1()
      m.update( user_fields['password'] )
      password_hash = m.hexdigest().lower()
      user_fields['password'] = password_hash
      
      # default constraints on a user's power
      user_fields['max_mdservers'] = self.api.DEFAULT_MAX_MDSERVERS
      user_fields['max_contents'] = self.api.DEFAULT_MAX_CONTENTS
      user_fields['enabled'] = False
      user_fields['roles'] = ['user']
      
      u = User( self.api, user_fields )
      
      # register this user on the CDN
      rc = self.api.cdn.add_user( u )
      if rc != 1:
         raise MDInternalError( "AddUser: could not add User(%s) to the CDN" % (u['username']) )
      
      u.sync()
      
      return u['user_id']
      