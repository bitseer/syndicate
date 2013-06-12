"""
SMDS authentication module for web2py.
Parts borrowed from /usr/share/python-support/python-gluon/gluon/tools.py
"""

import sys
sys.path.append( "/usr/share/syndicate_md" )
sys.path.append( "/usr/share/python-support/python-gluon/" )

import base64
import cPickle
import datetime
import thread
import logging
import sys
import os
import re
import time
import copy
import smtplib
import urllib
import urllib2
import Cookie
import cStringIO
from email import MIMEBase, MIMEMultipart, MIMEText, Encoders, Header, message_from_string

from gluon.contenttype import contenttype
from gluon.storage import Storage, StorageList, Settings, Messages
from gluon.utils import web2py_uuid
from gluon import *
from gluon.fileutils import read_file
from gluon.html import *

import gluon.serializers
import gluon.contrib.simplejson as simplejson

from SMDS.mdapi import MDAPI
from SMDS.auth import auth_user_from_email, auth_password_check
from SMDS.user import *
from SMDS.web2py.extras import SMDS_validators
import SMDS.logger as logger

from gluon.tools import Auth as GluonAuth

logger.init( "/tmp/SMDS_Auth.log" )

DEFAULT = lambda: None

def callback(actions,form,tablename=None):
    if actions:
        if tablename and isinstance(actions,dict):
            actions = actions.get(tablename, [])
        if not isinstance(actions,(list, tuple)):
            actions = [actions]
        [action(form) for action in actions]

def validators(*a):
    b = []
    for item in a:
        if isinstance(item, (list, tuple)):
            b = b + list(item)
        else:
            b.append(item)
    return b

def call_or_redirect(f,*args):
    if callable(f):
        redirect(f(*args))
    else:
        redirect(f)


def dict_to_Rows( my_dict ):
   extra_dict = {'_extra': my_dict}
   row = Row( extra_dict )
   rows = Rows( records=[row], colnames=list(my_dict.keys()), compact=False )
   return rows
   

class SMDS_Auth( GluonAuth ):
   """
   web2py Authentication module for SMDS
   """
   
   def __init__(self, api):
      """
      auth=Auth(globals(), db)

      - environment is there for legacy but unused (awful)
      - db has to be the database where to create tables for authentication

      """
      controller = 'default'
      cas_provider = None
      
      self.db = None
      self.environment = current
      request = current.request
      session = current.session
      auth = session.auth
      if auth and auth.last_visit and auth.last_visit + \
               datetime.timedelta(days=0, seconds=auth.expiration) > request.now:
         self.user = auth.user
         # this is a trick to speed up sessions
         if (request.now - auth.last_visit).seconds > (auth.expiration/10):
               auth.last_visit = request.now
      else:
         self.user = None
         session.auth = None
      settings = self.settings = Settings()

      # ## what happens after login?

      # ## what happens after registration?

      settings.hideerror = False
      settings.cas_domains = [request.env.http_host]
      settings.cas_provider = cas_provider
      settings.extra_fields = {}
      settings.actions_disabled = []
      settings.reset_password_requires_verification = False
      settings.registration_requires_verification = False
      settings.registration_requires_approval = True
      settings.alternate_requires_registration = False
      settings.create_user_groups = False

      settings.controller = controller
      settings.login_url = self.url('user', args='login')
      settings.logged_url = self.url('user', args='profile')
      settings.download_url = self.url('download')
      settings.mailer = None
      settings.login_captcha = None
      settings.register_captcha = None
      settings.retrieve_username_captcha = None
      settings.retrieve_password_captcha = None
      settings.captcha = None
      settings.expiration = 3600            # one hour
      settings.long_expiration = 3600*30*24 # one month
      settings.remember_me_form = False
      settings.allow_basic_login = False
      settings.allow_basic_login_only = False
      settings.on_failed_authorization = \
         self.url('user',args='not_authorized')

      settings.on_failed_authentication = lambda x: redirect(x)

      settings.formstyle = 'table3cols'
      settings.label_separator = ': '

      # ## table names to be used

      settings.password_field = 'password'
      settings.table_user_name = 'auth_user'
      settings.table_group_name = 'auth_group'
      settings.table_membership_name = 'auth_membership'
      settings.table_permission_name = 'auth_permission'
      settings.table_event_name = 'auth_event'
      settings.table_cas_name = 'auth_cas'

      # ## if none, they will be created

      settings.table_user = None
      settings.table_group = None
      settings.table_membership = None
      settings.table_permission = None
      settings.table_event = None
      settings.table_cas = None

      # ##

      settings.showid = False

      # ## these should be functions or lambdas

      settings.login_next = self.url('index')
      settings.login_onvalidation = []
      settings.login_onaccept = []
      settings.login_methods = [self]
      settings.login_form = self
      settings.login_email_validate = True
      settings.login_userfield = "username"

      settings.logout_next = self.url('index')
      settings.logout_onlogout = lambda x: None

      settings.register_next = self.url('index')
      settings.register_onvalidation = []
      settings.register_onaccept = []
      settings.register_fields = None

      settings.verify_email_next = self.url('user', args='login')
      settings.verify_email_onaccept = []

      settings.profile_next = self.url('index')
      settings.profile_onvalidation = []
      settings.profile_onaccept = []
      settings.profile_fields = None
      settings.retrieve_username_next = self.url('index')
      settings.retrieve_password_next = self.url('index')
      settings.request_reset_password_next = self.url('user', args='login')
      settings.reset_password_next = self.url('user', args='login')

      settings.change_password_next = self.url('index')
      settings.change_password_onvalidation = []
      settings.change_password_onaccept = []

      settings.retrieve_password_onvalidation = []
      settings.reset_password_onvalidation = []

      settings.hmac_key = None
      settings.lock_keys = True


      # ## these are messages that can be customized
      messages = self.messages = Messages(current.T)
      messages.login_button = 'Login'
      messages.register_button = 'Register'
      messages.password_reset_button = 'Request reset password'
      messages.password_change_button = 'Change password'
      messages.profile_save_button = 'Save profile'
      messages.submit_button = 'Submit'
      messages.verify_password = 'Verify Password'
      messages.delete_label = 'Check to delete:'
      messages.function_disabled = 'Function disabled'
      messages.access_denied = 'Insufficient privileges'
      messages.registration_verifying = 'Registration needs verification'
      messages.registration_pending = 'Registration is pending approval'
      messages.login_disabled = 'Login disabled by administrator'
      messages.logged_in = 'Logged in'
      messages.email_sent = 'Email sent'
      messages.unable_to_send_email = 'Unable to send email'
      messages.email_verified = 'Email verified'
      messages.logged_out = 'Logged out'
      messages.registration_successful = 'Registration successful'
      messages.invalid_email = 'Invalid email'
      messages.unable_send_email = 'Unable to send email'
      messages.invalid_login = 'Invalid login'
      messages.invalid_user = 'Invalid user'
      messages.invalid_password = 'Invalid password'
      messages.is_empty = "Cannot be empty"
      messages.mismatched_password = "Password fields don't match"
      messages.verify_email = 'A user wishes to join Syndicate.\nDetails:\n   Username: %(username)s\n   Email: %(email)s'
      messages.verify_email_subject = 'Email verification'
      messages.username_sent = 'Your username was emailed to you'
      messages.new_password_sent = 'A new password was emailed to you'
      messages.password_changed = 'Password changed'
      messages.retrieve_username = 'Your username is: %(username)s'
      messages.retrieve_username_subject = 'Username retrieve'
      messages.retrieve_password = 'Your password is: %(password)s'
      messages.retrieve_password_subject = 'Password retrieve'
      messages.reset_password = \
         'Click on the link http://...reset_password/%(key)s to reset your password'
      messages.reset_password_subject = 'Password reset'
      messages.invalid_reset_password = 'Invalid reset password'
      messages.profile_updated = 'Profile updated'
      messages.new_password = 'New password'
      messages.old_password = 'Old password'
      messages.group_description = \
         'Group uniquely assigned to user %(id)s'

      messages.register_log = 'User %(id)s Registered'
      messages.login_log = 'User %(id)s Logged-in'
      messages.login_failed_log = None
      messages.logout_log = 'User %(id)s Logged-out'
      messages.profile_log = 'User %(id)s Profile updated'
      messages.verify_email_log = 'User %(id)s Verification email sent'
      messages.retrieve_username_log = 'User %(id)s Username retrieved'
      messages.retrieve_password_log = 'User %(id)s Password retrieved'
      messages.reset_password_log = 'User %(id)s Password reset'
      messages.change_password_log = 'User %(id)s Password changed'
      messages.add_group_log = 'Group %(group_id)s created'
      messages.del_group_log = 'Group %(group_id)s deleted'
      messages.add_membership_log = None
      messages.del_membership_log = None
      messages.has_membership_log = None
      messages.add_permission_log = None
      messages.del_permission_log = None
      messages.has_permission_log = None
      messages.impersonate_log = 'User %(id)s is impersonating %(other_id)s'

      messages.label_first_name = 'First name'
      messages.label_last_name = 'Last name'
      messages.label_username = 'Username'
      messages.label_email = 'E-mail'
      messages.label_password = 'Password'
      messages.label_registration_key = 'Registration key'
      messages.label_reset_password_key = 'Reset Password key'
      messages.label_registration_id = 'Registration identifier'
      messages.label_role = 'Role'
      messages.label_description = 'Description'
      messages.label_user_id = 'User ID'
      messages.label_group_id = 'Group ID'
      messages.label_name = 'Name'
      messages.label_table_name = 'Table name'
      messages.label_record_id = 'Record ID'
      messages.label_time_stamp = 'Timestamp'
      messages.label_client_ip = 'Client IP'
      messages.label_origin = 'Origin'
      messages.label_remember_me = "Remember me (for 30 days)"
      messages['T'] = current.T
      messages.verify_password_comment = 'please input your password again'
      messages.lock_keys = True

      self.user = None
      self.api = api
      self.maint_email = api.config.MD_MAIL_SUPPORT_ADDRESS
      
      # disable stuff for now
      settings.actions_disabled.append('retrieve_username')
      settings.actions_disabled.append('retrieve_password')
      settings.actions_disabled.append('request_reset_password')
      settings.actions_disabled.append('profile')
      settings.actions_disabled.append('change_password')
      
      
      
   def _get_user_id(self):
      "accessor for auth.user_id"
      return (self.user and self.user.get('user_id')) or None
   user_id = property(_get_user_id, doc="user.id or None")
   
   def _HTTP(self, *a, **b):
      """
      only used in lambda: self._HTTP(404)
      """

      raise HTTP(*a, **b)

   def __call__(self):
      """
      usage:

      def authentication(): return dict(form=auth())
      """

      request = current.request
      args = request.args
      if not args:
         redirect(self.url(args='login',vars=request.vars))
      elif args[0] in self.settings.actions_disabled:
         raise HTTP(404)
      """
      if args[0] in ('login','logout','register','verify_email',
                     'retrieve_username','retrieve_password',
                     'reset_password','request_reset_password',
                     'change_password','profile','groups',
                     'impersonate','not_authorized'):
      """
      if args[0] in ('login','logout','register','not_authorized'):
         return getattr(self,args[0])()
      else:
         raise HTTP(404)

         
         
   def navbar(self,prefix='Welcome',action=None):
      """
      Create a pretty navigation bar
      """
      try:
         user = None
         session = current.session
         if session.auth:
            user = session.auth['user']
         
         request = current.request
         T = current.T
         if isinstance(prefix,str):
            prefix = T(prefix)
         if not action:
            action=URL(request.application,request.controller,'user')
         if prefix:
            prefix = prefix.strip()+' '
         
         if user:
            
            logout=A(T('logout'),_href=action+'/logout')
            profile=A(T('profile'),_href=action+'/profile')
            password=A(T('password'),_href=action+'/change_password')
            bar = SPAN(prefix, user['username'],' [ ', logout, ']',_class='auth_navbar')
            if not 'profile' in self.settings.actions_disabled:
                  bar.insert(4, ' | ')
                  bar.insert(5, profile)
            if not 'change_password' in self.settings.actions_disabled:
                  bar.insert(-1, ' | ')
                  bar.insert(-1, password)
         else:
            
            login=A(T('login'),_href=action+'/login')
            register=A(T('register'),_href=action+'/register')
            retrieve_username=A(T('forgot username?'),
                              _href=action+'/retrieve_username')
            lost_password=A(T('lost password?'),
                              _href=action+'/request_reset_password')
            bar = SPAN('[ ',login,' ]',_class='auth_navbar')

            if not 'register' in self.settings.actions_disabled:
                  bar.insert(2, ' | ')
                  bar.insert(3, register)
            if 'username' in User.public_fieldnames and not 'retrieve_username' in self.settings.actions_disabled:
                  bar.insert(-1, ' | ')
                  bar.insert(-1, retrieve_username)
            if not 'request_reset_password' in self.settings.actions_disabled:
                  bar.insert(-1, ' | ')
                  bar.insert(-1, lost_password)
         
         return bar
      except Exception, e:
         logger.exception(e, "Navbar error")
         logger.flush()
      
   
   def define_tables(self, username=None, migrate=None, fake_migrate=None):
      """ Do NOT define tables """
      pass
   
   
   def register(self, next=DEFAULT, onvalidation=DEFAULT, onaccept=DEFAULT, log=DEFAULT):
      """
      Register a new user
      """
      
      request = current.request
      response = current.response
      session = current.session
      
      if self.is_logged_in():
         # don't allow registration if we're already logged in
         redirect(self.settings.logged_url)
      
      # fill in defaults
      if next == DEFAULT:
         next = request.get_vars._next \
               or request.post_vars._next \
               or self.settings.register_next
      if onvalidation == DEFAULT:
         onvalidation = self.settings.register_onvalidation
      if onaccept == DEFAULT:
         onaccept = self.settings.register_onaccept
      if log == DEFAULT:
         log = self.messages.register_log

      # create a form...
      userfield = self.settings.login_userfield
      passfield = self.settings.password_field
      formstyle = self.settings.formstyle
      form =FORM(                                                                             \
                     TABLE(                                                                   \
                        TR(TD('Username:'),         TD(INPUT(_name="username",_type="text",requires=IS_SLUG(error_message="Invalid username")))),        \
                        TR(TD('Email:'),            TD(INPUT(_name="email", _type="text",requires=IS_EMAIL(error_message=self.messages.invalid_email)))),          \
                        TR(TD('Password:'),         TD(INPUT(_name="password", _type="password"))),    \
                        TR(TD('Re-type Password:'), TD(INPUT(_name="password2", _type="password",                         \
                                                       requires=IS_EXPR("value==%s" % repr(request.vars.get('password',None))),                   \
                                                       error_message=self.settings.mismatched_password)))    \
                     ),                                                                       \
                     INPUT(_type="Submit",_value="Register"),                                    \
                     _name="register"
               )
            

      if form.accepts(request, session, formname='register', onvalidation=onvalidation,hideerror=self.settings.hideerror):
         
         # verify that the password forms are the same
         if form.vars['password'] != form.vars['password2']:
            response.flash = messages.mismatched_password
            
         # inform the admin
         """
         if not self.settings.mailer or \
            not self.settings.mailer.send(
               to=self.maint_email,
               subject=self.messages.verify_email_subject,
               message=self.messages.verify_email % dict(username=form.vars['username'], email=form.vars['email'])):
                     
            response.flash = self.messages.unable_send_email
            return form
            
         session.flash = self.messages.email_sent
         """
         
         # make sure this user does not exist
         rc = 0
         msg = ""
         try:
            user = Users(self.api, {'username': form.vars['username']})[0]
            rc = -1     # already exists
            msg = "User already exists"
         except:
            pass
            
         # create the user
         if rc == 0:
            try:
               user_fields = {'username': form.vars['username'], 'password': form.vars['password'], 'email': form.vars['email']}
               rc = self.api.call( ("127.0.0.1", "localhost"), "AddUser", self.api.maint_auth, user_fields )
            except Exception, e:
               logger.exception(e, "register: exception")
               logger.flush()
               msg = "User could not be registered"
               rc = -1
         
         if rc < 0:
            response.flash = msg
            logger.error("Failed to add user '%s' (email '%s')" % (user_fields['username'], user_fields['email']) )
            return form
            
         session.flash = self.messages.registration_pending
         if log:
            logger.info("Added user '%s' (email '%s')" % (user_fields['username'], user_fields['email']) )
         
         callback(onaccept,form)
         if not next:
            next = self.url(args = request.args)
         elif isinstance(next, (list, tuple)): ### fix issue with 2.6
            next = next[0]
         elif next and not next[0] == '/' and next[:4] != 'http':
            next = self.url(next.replace('[id]', str(form.vars.id)))
         redirect(next)
         
      return form
      
   
   def login_bare( self, username, password ):
      """
      Bare essentials login.
      """
      api = MDAPI()
      
      user = None
      try:
         user = auth_user_from_email( api, username )
      except Exception, e:
         logger.error( "User '%s' could not be authenticated (exception = %s)" % (username, e) )
         return False
      
      
      rc = False
      auth_struct = {'AuthMethod': 'password', 'Username': user['username'], 'AuthString': password}
      
      try:
         rc = auth_password_check( api, auth_struct, user, None )
      except Exception, e:
         logger.error( "User '%s' failed to authenticate" % username)

      if rc and user:
         user_public = user.public()
         user_stored = Storage(user_public)

         if log:
            logger.info("SMDS_Auth: User '%s' logged in" % user_public['username'])

         # process authenticated users
         # user wants to be logged in for longer
         session.auth = Storage(
               user = user_stored,
               last_visit = request.now,
               expiration = self.settings.expiration,
               hmac_key = web2py_uuid()
               )

         self.user = user_public
         logger.info("SMDS_Auth: user_id = %s" % self.user_id)
         logger.flush()
         
         return user
         
      return rc
      
      
   def login(self, next=DEFAULT, onvalidation=DEFAULT, onaccept=DEFAULT, log=DEFAULT):
      """
      Handle a login request, and redirect.
      """
      request = current.request
      response = current.response
      session = current.session
      
      username_field = self.settings.login_userfield
      password_field = self.settings.password_field
      
      if next == DEFAULT:
         next = request.get_vars._next \
            or request.post_vars._next \
            or self.settings.login_next
                
      if onvalidation == DEFAULT:
         onvalidation = self.settings.login_onvalidation
      if onaccept == DEFAULT:
         onaccept = self.settings.login_onaccept
      if log == DEFAULT:
         log = self.messages.login_log
      
      user = None
      accepted_form = False
      
      if self.settings.login_form == self:
         # this object was responsible for logging in
         form =FORM(                                                                          \
                     TABLE(                                                                   \
                        TR(TD('Username:'), TD(INPUT(_name="username",_type="text",requires=IS_SLUG(error_message="Invalid Username")))),        \
                        TR(TD('Password:'), TD(INPUT(_name="password", _type="password")))    \
                     ),                                                                       \
                     INPUT(_type="Submit",_value="Login"),                                    \
                     _name="login"
               )
            

         if form.accepts(request.vars, session,
                         formname='login',
                         onvalidation=onvalidation,
                         hideerror=self.settings.hideerror):
            
            # sanitize inputs
            
            accepted_form = True
            
            # check for username in db
            username = form.vars[username_field]
            user = None
            try:
               user = Users( self.api, {'username': username} )[0]
            except:
               pass
               
            if user:
               # user in db, check if registration pending or disabled
               temp_user = user
               if temp_user['enabled'] == False:
                  # user is not yet enabled
                  response.flash = self.messages.login_disabled
                  return form
                  
               # check password
               try:
                  rc = auth_password_check( self.api, {'Username':user['username'], 'AuthMethod':'password', 'AuthString':form.vars[password_field]}, user, None )
               except:
                  if log:
                     logger.error("SMDS_Auth: User '%s' authentication failed (invalid credentials)" % user['username'] )
                     logger.flush()
                     
                  user = None   # invalid credentials
               
            if not user:
               if log:
                  logger.error("SMDS_Auth: User could not be looked up" )
                  logger.flush()
                  
               # invalid login
               session.flash = self.messages.invalid_login
               redirect(self.url(args=request.args,vars=request.get_vars))

      if user:
         user_public = user.public()
         user_stored = Storage(user_public)

         if log:
            logger.info("SMDS_Auth: User '%s' logged in" % user_public['username'])

         # process authenticated users
         # user wants to be logged in for longer
         session.auth = Storage(
               user = user_stored,
               last_visit = request.now,
               expiration = self.settings.long_expiration,
               remember = request.vars.has_key("remember"),
               hmac_key = web2py_uuid()
               )

         self.user = user_public
         logger.info("SMDS_Auth: user_id = %s" % self.user_id)
         logger.flush()
         
         session.flash = self.messages.logged_in

      # how to continue
      if self.settings.login_form == self:
         if accepted_form:
            callback(onaccept,form)
            if isinstance(next, (list, tuple)):
               # fix issue with 2.6
               next = next[0]
            if next and not next[0] == '/' and next[:4] != 'http':
               next = self.url(next.replace('[id]', str(form.vars.id)))
            
            redirect(next)
         
         return form
      elif user:
         callback(onaccept,None)
      
      redirect(next)
      
   
   def logout(self, next=DEFAULT, onlogout=DEFAULT, log=DEFAULT):
      """
      Handle a logout
      """
      
      session = current.session
      user = None
      if session.auth:
         user = session.auth['user']
         self.user = user
      
      if log:
         if user:
            logger.info("SMDS_Auth: User '%s' logged out" % user['username'])
            logger.flush()
      
      next = self.settings.logout_next

      #super(SMDS_Auth, self).logout( lambda x: redirect(self.url('index')), lambda x, log )
      
      if next == DEFAULT:
         next = self.settings.logout_next
      """
      if onlogout == DEFAULT:
         onlogout = self.settings.logout_onlogout
      if onlogout:
         onlogout(self.user)
      if log == DEFAULT:
         log = self.messages.logout_log
      if log and self.user:
         self.log_event(log % self.user)
      
      if self.settings.login_form != self:
         cas = self.settings.login_form
         cas_user = cas.get_user()
         if cas_user:
            next = cas.logout_url(next)
      """
      current.session.auth = None
      current.session.flash = self.messages.logged_out
      if next:
         redirect(next)
      
   
   def requires_login(self):
      """
      decorator that prevents access to action if not logged in
      """

      def decorator(action):

         def f(*a, **b):

               if self.settings.allow_basic_login_only and not self.basic():
                  if current.request.is_restful:
                     raise HTTP(403,"Not authorized")
                  return call_or_redirect(self.settings.on_failed_authorization)

               if not self.basic() and not current.session.auth: #self.is_logged_in():
                  if current.request.is_restful:
                     raise HTTP(403,"Not authorized")
                  request = current.request
                  next = URL(r=request,args=request.args,
                              vars=request.get_vars)
                  current.session.flash = current.response.flash
                  return call_or_redirect(
                     self.settings.on_failed_authentication,
                     self.settings.login_url + '?_next='+urllib.quote(next)
                     )
               return action(*a, **b)
         f.__doc__ = action.__doc__
         f.__name__ = action.__name__
         f.__dict__.update(action.__dict__)
         return f

      return decorator
      
   
   def profile(self, next=DEFAULT, onvalidation=DEFAULT, onaccept=DEFAULT, log=DEFAULT):
      pass 
   
   def change_password(self, next=DEFAULT, onvalidation=DEFAULT, onaccept=DEFAULT, log=DEFAULT):
      pass 
   
   def verify_email(self, next=DEFAULT, onaccept=DEFAULT, log=DEFAULT ):
      pass 
   
   def retrieve_username(self, next=DEFAULT, onvalidation=DEFAULT, onaccept=DEFAULT, log=DEFAULT ):
      pass 
   
   def request_reset_password( self, next=DEFAULT, onvalidation=DEFAULT, onaccept=DEFAULT, log=DEFAULT ):
      pass
   
   def reset_password( self, next=DEFAULT, onvalidation=DEFAULT, onaccept=DEFAULT, log=DEFAULT ):
      pass 
   
   def impersonate( self, user_id=DEFAULT ):
      pass
   
   def groups( self ):
      pass
   
   def not_authorized( self ):
      """ YOU SHALL NOT PASS """
      return 'ACCESS DENIED'



def SMDS_authentication( logfile="/tmp/SMDS_login.log" ):
    """
    Authenticate with the Syndicate metadata service
    """
    logger.init( open(logfile, "a") )
    
    def SMDS_auth_aux(username, password):
      
      api = MDAPI()
      
      user = None
      try:
         user = auth_user_from_email( api, username )
      except Exception, e:
         logger.error( "User '%s' could not be authenticated (exception = %s)" % (username, e) )
         return False
      
      
      rc = False
      auth_struct = {'AuthMethod': 'password', 'Username': user['username'], 'AuthString': password}
      
      try:
         rc = auth_password_check( api, auth_struct, user, None )
      except Exception, e:
         logger.error( "User '%s' failed to authenticate" % username)

      return rc

    return SMDS_auth_aux
