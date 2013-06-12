"""
Class that describes a CDN
"""

from SMDS.user import *
from SMDS.content import *

class MDCDN:
   """
   Interface for communicating with a CDN's brain to add/remove users and content.
   """
   
   def __init__(self):
      pass
   
   def setup(self, api):
      return 1
   
   def shutdown(self):
      return 1
   
   def add_user( self, user ):
      return 1
   
   def add_content( self, user, content ):
      return 1
   
   def rm_user( self, user ):
      return 1
   
   def rm_content( self, content ):
      return 1
   
   def get_users( self ):
      return None
   
   def get_contents( self ):
      return None
   
   def update_user( self, user ):
      return 1
   
   def update_content( self, content ):
      return 1