#!/usr/bin/python

from SMDS.method import Method
from SMDS.mdserver import *
from SMDS.user import *
from SMDS.parameter import *
from SMDS.auth import Auth
from SMDS.faults import *

class UpdateUser( Method ):
   """
   Update a user.  A user can only update himself/herself, and only a few fields at that.  An admin can update anyone, with some admin-specific fields.
   """
   
   admin_only_fields = [
         'max_mdservers',
         'max_contents',
         'enabled',
         'roles',
         'username'
   ]
   
   accepts = [
         Auth(),
         Mixed( User.fields['username'], User.fields['user_id'] ),
         dict([(n,User.fields[n]) for n in set(User.fields.keys()).difference(set(['user_id']))])
   ]
   roles = ['admin','user']
   returns = Parameter(int, "1 if successful; negative error code otherwise")
   
   def call(self, auth, username_or_id, user_fields):
      assert self.caller is not None
      
      roles = self.caller['roles']
      
      # look up the caller user ID
      calling_users = Users( self.api, {'username': auth['Username']} )
      calling_user = calling_users[0]
      
      # look up this user
      user = None
      try:
         users = None
         if isinstance( username_or_id, str ):
            users = Users( self.api, {'username': username_or_id} )
         else:
            users = Users( self.api, {'user_id': username_or_id} )
            
         user = users[0]
      except Exception, e:
         raise MDObjectNotFound( "User(%s)" % username_or_id, e )
      
      # can we update this user?
      if ('admin' not in roles) and user['user_id'] != calling_user['user_id']:
         raise MDUnauthorized( "User(%s) cannot be updated by User(%s)" % (username_or_id, Auth['Username']) )
      
      if 'admin' not in roles:
         # not an admin, so make sure that the admin-only fields don't change
         bad_fields = []
         for aof in self.admin_only_fields:
            if aof in user_fields.keys():
               bad_fields.append( aof )
            
         
         if len(bad_fields) > 0:
            raise MDUnauthorized( "Only an admin can update fields %s of User(%s)" % (username_or_id, ", ".join(bad_fields)) )
         
         
      if 'password' in user_fields.keys():
         # hash the password
         m = Auth.new_sha1()
         m.update( user_fields['password'] )
         password_hash = m.hexdigest().lower()
         user_fields['password'] = password_hash
      
      user.update( user_fields )
      user.sync()
      
      return 1
      