#!/usr/bin/python

from SMDS.API.CDN import CDN
from SMDS.mdapi import MDAPI
import traceback
import xmlrpclib

import SMDS.logger as logger

class VerivueCoBlitz( CDN.MDCDN ):
   
   def setup(self, api):
      self.api = api
      
      self.CDN_auth = {
         "AuthMethod": "password",
         "Username": api.config.MD_CDN_USERNAME,
         "AuthString": api.config.MD_CDN_PASSWORD,
      }
      
      # read the SFA credential, if it exists
      try:
         credf = open(api.config.MD_SFA_CREDENTIAL)
         credstr = credf.read()
         credf.close()
         
         self.CDN_auth["SFA_DelegatedCred"] = credstr
      except:
         logger.warn("VerivueCoBlitz: no SFA credential given")
      
      rc = None
      
      # connect to the CMI
      self.cdn_api = xmlrpclib.ServerProxy( api.config.MD_CDN_API_URL, allow_none = True )
      
      # get our content provider struct
      try:
         self.content_provider = self.cdn_api.Read( self.CDN_auth, "ContentProvider", api.config.MD_CONTENT_PROVIDER )[0]
         rc = 1
      except Exception, e:
         try:
            logger.warn("VerivueCoBlitz(setup): new API failed; trying old API")
            self.content_provider = self.cdn_api.GetContentProviders( self.CDN_auth, {'account': api.config.MD_CONTENT_PROVIDER} )[0]
            rc = 1
         except Exception, e2:
            logger.exception(e2, "VerivueCoBlitz(setup): could not look up SMDS CDN content provider account '%s'" % api.config.MD_CONTENT_PROVIDER )
            rc = None
      
      if rc == 1:
         logger.info("VerivueCoBlitz: connected to %s" % api.config.MD_CDN_API_URL )
      return rc
      
   
   def shutdown(self):
      return 1
   
   def add_user( self, user ):
      return 1
   
   def add_content( self, user, content_url ):
      content_info = {
         "url":                  content_url,
         "enabled":              True,
         "description":          "Created by SMDS",
         "content_provider_id":  self.content_provider['content_provider_id']
      }
      
      rc = None
      try:
         # try the new API (works with CoSFA)
         rc = self.cdn_api.Create( self.CDN_auth, "Content", content_info )
         
      except Exception, e:
         traceback.print_exc()
         # possibly using the old API
         try:
            logger.warn("VerivueCoBlitz(add_content): new API failed; trying old API")
            rc = self.cdn_api.AddContent( self.CDN_auth, content_info['content_provider_id'], content_info )
         except Exception, e2:
            logger.exception( e2, "VerivueCoBlitz(add_content): could not add content '%s'" % content_url )
         
      
      return rc
         
   
   def rm_user( self, user ):
      return 1
   
   def rm_content( self, content_id ):
      rc = None
      try:
         # try the new API (works with CoSFA)
         rc = self.cdn_api.Delete( self.CDN_auth, "Content", content_id )
         
      except Exception, e:
         traceback.print_exc()
         
         # possibly using the old API
         try:
            logger.warn("VerivueCoBlitz(rm_contet): new API failed; trying old API")
            rc = self.cdn_api.DeleteContent( self.CDN_auth, content_id )
         except Exception, e2:
            logger.exception( e2, "VerivueCoBlitz(rm_content): could not remove content '%s'" % content_id )
         
      return rc
   
   def get_users( self ):
      return 1
   
   def get_contents( self, filter_=None, ret=None  ):
      content = None
      try:
         # try the new API (works with CoSFA)
         content = self.cdn_api.ListAll( self.CDN_auth, "Content", filter_, ret )
      except Exception, e:
         # possibly using the old API
         try:
            logger.warn("VerivueCoBlitz: new API failed; trying old API")
            content = self.cdn_api.GetContents( self.CDN_auth, filter_, ret )
         
         except Exception, e:
            logger.exception( e, "Could not get content")
         
      return content
   
   def update_user( self, user ):
      return 1
   
   def update_content( self, content_id, content_info ):
      rc = None
      try:
         # try the new API (works with CoSFA)
         rc = self.cdn_api.Update( self.CDN_auth, "Content", content_id, content_info )
         
      except Exception, e:
         traceback.print_exc()
         
         # possibly using the old API
         try:
            logger.warn("VerivueCoBlitz(update_content): new API failed; trying old API")
            rc = self.cdn_api.UpdateContent( self.CDN_auth, content_id, content_info )
         except Exception, e2:
            logger.exception( e2, "VerivueCoBlitz(update_content): could not update content '%s'" % content_id )
            
      return 1
   
   
   
CDNDriver = VerivueCoBlitz