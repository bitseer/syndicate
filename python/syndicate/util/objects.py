#!/usr/bin/env python

"""
   Copyright 2015 The Trustees of Princeton University

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
"""

# methods for loading and processing object parameters

import syndicate.ms.msconfig as msconfig
from syndicate.ms.msconfig import *
import syndicate.protobufs.ms_pb2 as ms_pb2
import syndicate.protobufs.sg_pb2 as sg_pb2
from syndicate.ms.jsonrpc import json_stable_serialize
import syndicate.util.storage as storagelib
import syndicate.util.config as conf

import binascii
import inspect
import re 
import sys
import base64
import random
import json
import ctypes
import zlib
import socket
import fnmatch
import hashlib
import datetime
import time
import traceback

from Crypto.Hash import SHA256 as HashAlg
from Crypto.PublicKey import RSA as CryptoKey
from Crypto import Random
from Crypto.Signature import PKCS1_PSS as CryptoSigner
from Crypto.Protocol.KDF import PBKDF2


log = conf.log

SECRETS_PAD_KEY = "__syndicate_pad__"

# RFC-822 compliant, as long as there aren't any comments in the address.
# taken from http://chrisbailey.blogs.ilrt.org/2013/08/19/validating-email-addresses-in-python/
email_regex_str = r"^(?=^.{1,256}$)(?=.{1,64}@)(?:[^\x00-\x20\x22\x28\x29\x2c\x2e\x3a-\x3c\x3e\x40\x5b-\x5d\x7f-\xff]+|\x22(?:[^\x0d\x22\x5c\x80-\xff]|\x5c[\x00-\x7f])*\x22)(?:\x2e(?:[^\x00-\x20\x22\x28\x29\x2c\x2e\x3a-\x3c\x3e\x40\x5b-\x5d\x7f-\xff]+|\x22(?:[^\x0d\x22\x5c\x80-\xff]|\x5c[\x00-\x7f])*\x22))*\x40(?:[^\x00-\x20\x22\x28\x29\x2c\x2e\x3a-\x3c\x3e\x40\x5b-\x5d\x7f-\xff]+|[\x5b](?:[^\x0d\x5b-\x5d\x80-\xff]|\x5c[\x00-\x7f])*[\x5d])(?:\x2e(?:[^\x00-\x20\x22\x28\x29\x2c\x2e\x3a-\x3c\x3e\x40\x5b-\x5d\x7f-\xff]+|[\x5b](?:[^\x0d\x5b-\x5d\x80-\xff]|\x5c[\x00-\x7f])*[\x5d]))*$"

email_regex = re.compile( email_regex_str )


def encrypt_secrets_dict( gateway_privkey_pem, secrets_dict ):
   """
   Encrypt a secrets dictionary, returning the serialized base64-encoded ciphertext.
   """
   
   try:
      import syndicate.syndicate as c_syndicate
   except Exception, e:
      log.exception(e)
      raise Exception("Failed to load libsyndicate")
   
   # pad the secrets first
   # (NOTE: we're also relying on the underlying cryptosystem's padding, but this can't hurt).
   secrets_dict[ SECRETS_PAD_KEY ] = base64.b64encode( ''.join(chr(random.randint(0,255)) for i in xrange(0,256)) )
   
   try:
      secrets_dict_str = json_stable_serialize( secrets_dict )
   except Exception, e:
      log.exception( e )
      raise Exception("Failed to serialize secrets")
   
   try:
      privkey = CryptoKey.importKey( gateway_privkey_pem )
   except Exception, e:
      log.exception(e)
      raise Exception("Failed to parse private key" )
      
   pubkey_pem = privkey.publickey().exportKey()
   
   # encrypt the serialized secrets dict 
   rc = 0
   encrypted_secrets_str = None
   try:
      log.info("Encrypting secrets...")
      rc, encrypted_secrets_str = c_syndicate.encrypt_data( gateway_privkey_pem, pubkey_pem, secrets_dict_str )
   except Exception, e:
      log.exception( e )
      raise Exception("Failed to encrypt secrets")
   
   if rc != 0 or encrypted_secrets_str is None:
      raise Exception("Failed to encrypt secrets, rc = %d" % rc)
   
   return encrypted_secrets_str


def load_driver_secrets( secrets_path, gateway_privkey_pem ):
   """
   Given the path to our driver on disk, load and serialize the secrets dictionary,
   signing it with the gateway's private key, and encrypting it with the 
   gateway's public key.
   
   Return serialized encrypted secrets
   Return None if there are no secrets for this driver
   """
   
   secrets_json = None 

   if not os.path.exists( secrets_path ):
      # no secrets 
      return None

   with open( secrets_path, "r" ) as f:
      secrets_json = f.read()
   
   secrets_dict = {}
   
   # verity that it's a JSON doc 
   try:
      secrets_dict = json.loads( secrets_json )
   except Exception, e:
      raise Exception("Not a JSON document: '%s'" % secrets_path )
   
   encrypted_secrets_str = encrypt_secrets_dict( gateway_privkey_pem, secrets_dict )
   return encrypted_secrets_str



def load_driver( driver_path, gateway_privkey_pem ):
   """
   Load a driver.
   
   Each file in the driver_path will be incorporated as a base64-encoded string
   in a JSON document, keyed by its filename.
   
   If the file is called 'secrets', it will be signed and encrypted by the 
   gateway's key pair, under the key "secrets".
   
   Returns a dict with the base64-encoded contents of each file in driver_path
   """
   
   driver = {}
   
   for filename in os.listdir( driver_path ):
      
      path = os.path.join( driver_path, filename )
      data = None
      if filename == 'secrets':
         data = load_driver_secrets( path, gateway_privkey_pem )
      
      else:
         with open( path, "r" ) as f:
            data = f.read()
         
      # serialize...
      datab64 = base64.b64encode( data )
      driver[ filename ] = datab64 
   
   return driver
   
   
def hash_data( data ):
   """
   Given a string of data, calculate 
   the SHA256 over it
   """
   h = HashAlg.new()
   h.update( data )
   return h.digest()

   
def sign_data( privkey, data ):
   """
   Given a loaded private key and a string of data,
   generate and return a signature over it.
   """
   h = HashAlg.new( data )
   signer = CryptoSigner.new(privkey)
   signature = signer.sign( h )
   return signature 


def load_id( config, object_type, object_name ):
   """
   Load a .id file to recover the numeric ID of the object on the MS.
   Return the numeric ID on success.
   Return None on error 
   """
   
   id_path = conf.object_file_path( config, object_type, object_name + ".id" )
   
   try:
      with open( id_path, "r" ) as f:
         numeric_id_str = f.read().strip()
         numeric_id = int( numeric_id_str )
         return numeric_id
   
   except Exception, e:
      return None 
   

def store_id( config, object_type, object_name, numeric_id ):
   """
   Store a numeric ID to an object's .id file.
   Return True on success 
   Return False on error
   """
   
   id_path = conf.object_file_path( config, object_type, object_name + ".id" )
   
   try:
      with open( id_path, "w") as f:
         f.write( "%s\n" % str(numeric_id) )
         return True 
      
   except Exception, e:
      return False 


def remove_id( config, object_type, object_name ):
   """
   Remove a numeric ID for an object.
   """
   
   id_path = conf.object_file_path( config, object_type, object_name + ".id" )
   
   try:
      os.unlink( id_path )
   except Exception, e:
      pass 
  

def find_by_id( config, object_type, owner_id ):
   """
   Find a named object, given its ID.
   WARNING: Expensive; don't use unless you have to.
   
   Return the username on success 
   Return None on error or not found
   """
   
   id_dir = os.path.dirname( conf.object_file_path( config, object_type, "ignored" ) )
   
   for id_filename in os.listdir( id_dir ):
      
      id_path = os.path.join( id_dir, id_filename )
      
      # an id file?
      if not id_path.endswith(".id"):
         continue
      
      id_str = None
      with open(id_path, "r") as f:
         id_str = f.read()
         
      try:
         if owner_id == int( id_str.strip() ):
            
            # filename encodes the name
            return id_filename[:-3]
      
      except:
         # could not parse 
         pass
      
   return None
   
   
def load_name( config, object_type, object_id ):
   """
   Load a .name file to recover the name of the object given on the MS.
   Return the name on success.
   Return None on error 
   """
   
   id_path = conf.object_file_path( config, object_type, str(object_id) + ".name" )
   
   try:
      with open( id_path, "r" ) as f:
         name = f.read().strip()
         return name
   
   except Exception, e:
      return None 
   

def link_id_cert( config, object_type, numeric_id, object_name ):
   """
   create a hard link to an object's cert using its numeric ID
   Return True on success 
   Return False on error
   """
   
   id_path = conf.object_file_path( config, object_type, str(numeric_id) + ".cert" )
   cert_path = conf.object_file_path( config, object_type, object_name + ".cert" )
   
   try:
      os.link( cert_path, id_path )
      return True 
  
   except Exception, e:
      return False 


def unlink_id_cert( config, object_type, numeric_id ):
   """
   remove a hard link to an object's cert from its numeric ID
   """
   
   id_path = conf.object_file_path( config, object_type, str(numeric_id) + ".cert" )
   
   try:
      os.unlink( id_path )
  
   except Exception, e:
      pass 
  
  
def clock_gettime():
   """
   Get the current time, in seconds and nanosecons,
   since the epoch.
   """
   now = datetime.datetime.utcnow()
   nowtt = now.timetuple()
   now_sec = int(time.mktime( nowtt ))
   now_nsec = int(now.microsecond * 1e3)
   return (now_sec, now_nsec)
   
   
def make_volume_root( volume_cert ):
   """
   Generate a root directory inode for the volume.
   Add it to volume_cert.root
   """
   
   now_sec, now_nsec = clock_gettime()
   
   volume_cert.root.type = ms_pb2.ms_entry.MS_ENTRY_TYPE_DIR
   volume_cert.root.file_id = 0
   volume_cert.root.ctime_sec = now_sec
   volume_cert.root.ctime_nsec = now_nsec 
   volume_cert.root.mtime_sec = now_sec 
   volume_cert.root.mtime_nsec = now_nsec 
   volume_cert.root.manifest_mtime_sec = now_sec 
   volume_cert.root.manifest_mtime_nsec = now_nsec 
   volume_cert.root.owner = volume_cert.owner_id 
   volume_cert.root.coordinator = 0
   volume_cert.root.volume = volume_cert.volume_id 
   volume_cert.root.mode = 0700
   volume_cert.root.size = 4096         # compatibility with most filesystem types 
   volume_cert.root.version = 1
   volume_cert.root.max_read_freshness = 5000           # 5 seconds 
   volume_cert.root.max_write_freshness = 5000          # 5 seconds 
   volume_cert.root.name = "/"
   volume_cert.root.write_nonce = 1
   volume_cert.root.xattr_nonce = 1
   volume_cert.root.generation = 1
   volume_cert.root.parent_id = 0
   volume_cert.root.num_children = 0
   volume_cert.root.capacity = 16
   volume_cert.root.signature = ""
   
   return
   
   
def load_volume_cert( config, volume_name ):
   """
   Load a volume cert from disk, owned by the given user.
   """
   
   volume_cert_pb = None 
   volume_cert_path = conf.object_file_path( config, "volume", volume_name + ".cert" )
   
   if not os.path.exists( volume_cert_path ):
      return None 
   
   with open(volume_cert_path, "r" ) as f:
      volume_cert_pb = f.read()
      
   try:
      volume_cert = ms_pb2.ms_volume_metadata() 
      volume_cert.ParseFromString( volume_cert_pb )
   except Exception, e:
      log.error("Unparseable volume cert '%s'" % volume_cert_path )
      return None 
   
   return volume_cert


def store_volume_cert( config, volume_cert ):
   """
   Given a deserialized volume cert, serialize and store it.
   Return True on success
   """
   
   volume_cert_pb = volume_cert.SerializeToString()
   volume_cert_path = conf.object_file_path( config, "volume", volume_cert.name + ".cert" )
   
   with open(volume_cert_path, "w") as f:
      f.write( volume_cert_pb )
      f.flush()
   
   return True


def remove_volume_cert( config, volume_name ):
   """
   Given the name of a volume, remove its cert.
   """
   
   volume_cert_path = conf.object_file_path( config, "volume", volume_name + ".cert" )
   
   try:
      os.unlink( volume_cert_path )
   except:
      pass
   

def load_gateway_cert( config, gateway_name ):
   """
   Given the config dict and the name of the gateway,
   go load its cert if it's on file.
   
   Return the deserialized cert on success.
   Return None if not found or if corrupt.
   """
   
   gateway_cert_pb = None 
   gateway_cert_path = conf.object_file_path( config, "gateway", gateway_name + ".cert" )
   
   if not os.path.exists( gateway_cert_path ):
      return None 
   
   with open(gateway_cert_path, "r") as f:
      gateway_cert_pb = f.read()
   
   try:
      gateway_cert = ms_pb2.ms_gateway_cert()
      gateway_cert.ParseFromString( gateway_cert_pb )
   except Exception, e:
      log.error("Unparseable gateway cert '%s'" % gateway_cert_path)
      return None
   
   return gateway_cert


def store_gateway_cert( config, gateway_cert ):
   """
   Given a gateway cert, store it to disk.
   
   Return True on success 
   """
   
   gateway_cert_pb = gateway_cert.SerializeToString()
   gateway_cert_path = conf.object_file_path( config, "gateway", gateway_cert.name + ".cert" )
   
   with open(gateway_cert_path, "w") as f:
      f.write( gateway_cert_pb )
   
   return True
   
   
def remove_gateway_cert( config, gateway_name ):
   """
   Remove a gateway cert.
   """
   
   gateway_cert_path = conf.object_file_path( config, "gateway", gateway_name + ".cert" )
   
   try:
      os.unlink( gateway_cert_path )
   except:
      pass 
   

def load_user_cert( config, email ):
   """
   Given the config dict and email,
   go load the user's on-file cert.
   
   Return the deserialized cert on success.
   Return None if not found or corrupt.
   """
   
   user_cert_pb = None 
   user_cert_path = conf.object_file_path( config, "user", email + ".cert" )
   
   if not os.path.exists( user_cert_path ):
      return None 
   
   with open(user_cert_path, "r") as f:
      user_cert_pb = f.read()
   
   try:
      user_cert = ms_pb2.ms_user_cert()
      user_cert.ParseFromString( user_cert_pb )
   except Exception, e:
      log.error("Unparseable user cert '%s'" % user_cert_path )
      return None 
   
   return user_cert 


def store_user_cert( config, user_cert ):
   """
   Given a user cert, store it to disk.
   
   Return True on success
   """
   
   user_cert_pb = user_cert.SerializeToString()
   user_cert_path = conf.object_file_path( config, "user", user_cert.email + ".cert" )
   
   with open( user_cert_path, "w" ) as f:
      f.write( user_cert_pb )
   
   return True 


def remove_user_cert( config, email ):
   """
   Remove a user cert 
   """
   
   user_cert_path = conf.object_file_path( config, "user", email + ".cert" )
   
   try:
      os.unlink( user_cert_path )
   except:
      pass 
   

def make_volume_cert_bundle( config, volume_owner, volume_name, volume_id=None, new_volume_cert=None, new_gateway_cert=None ):
   """
   Given the name of a volume, generate and return 
   a serialized protobuf signed with the volume owner's
   private key that describes which gateways exist in the volume.
   It will set the cert bundle timestamp to the current UTC time;
   it is assumed that the local clock increments monotonically
   "enough" that each cert bundle generated 
   will have a monotonically-increasing timestamp.
   
   Optionally use the new_volume_cert to override the on-disk 
   volume cert.  This is useful if we're in the process of 
   creating a volume.
   
   Optionally use the new_gateway_cert to override one 
   potentially-existing gateway certificate.  This is useful 
   if we're in the process of creating or updating a gateway,
   and we need to include this new gateway certificate in the 
   cert bundle.
   
   Return the serialized protobuf on success 
   Return None on error
   """
   
   owner_privkey = storagelib.load_private_key( config, "user", volume_owner )
   if owner_privkey is None:
      log.error("Failed to load private key for '%s'" % volume_owner )
      return None 
   
   owner_id = load_id( config, "user", volume_owner )
   if owner_id is None:
      log.error("Failed to load user ID for '%s'" % volume_owner )
      return None 
   
   if volume_id is None:
      volume_id = load_id( config, "volume", volume_name )
      if volume_id is None:
         log.error("Failed to load ID for volume '%s'" % volume_name )
         return None 
   
   now_sec, now_nsec = clock_gettime()
   
   if new_volume_cert is None:
      volume_cert = load_volume_cert( config, volume_name )
   else:
      volume_cert = new_volume_cert
   
   # sanity check...
   if owner_id != volume_cert.owner_id:
      log.error("Volume cert owner ID mismatch: %s != %s" % (owner_id, volume_cert.owner_id))
      return None
      
   if volume_id != volume_cert.volume_id:
      log.error("Volume ID mismatch: %s != %s" % (volume_id, volume_cert.volume_id))
      return None 
   
   cert_manifest = sg_pb2.Manifest() 
   
   # duplicate information from the volume cert to the cert manifest header
   cert_manifest.volume_id = volume_cert.volume_id
   cert_manifest.coordinator_id = 0
   cert_manifest.file_id = 0
   cert_manifest.owner_id = volume_cert.owner_id
   cert_manifest.file_version = volume_cert.volume_version
   cert_manifest.mtime_sec = now_sec         # serves as the cert version
   cert_manifest.mtime_nsec = 0
   
   # map gateway ID to cert and serialized cert
   certs = {}
   certs_pb = {}
   used_new_gateway_cert = False
   
   # find each gateway 
   gateway_cert_dir = os.path.dirname( conf.object_file_path( config, "gateway", "ignored" ) )
   
   for gateway_cert_filename in os.listdir( gateway_cert_dir ):
      
      # gateway cert?
      if not gateway_cert_filename.endswith(".cert"):
         continue 
      
      gateway_name = gateway_cert_filename[:-5]
      
      # are we given this cert already?
      if new_gateway_cert is not None and new_gateway_cert.name == gateway_name:
         gateway_cert = new_gateway_cert
         used_new_gateway_cert = True
         
      else:
         gateway_cert = load_gateway_cert( config, gateway_name )
      
      if gateway_cert.volume_id != volume_id:
         # not in this volume 
         continue 
      
      certs[gateway_cert.gateway_id] = gateway_cert
      certs_pb[gateway_cert.gateway_id] = gateway_cert.SerializeToString()
      
   # the new_gateway_cert might be completely new...
   if not used_new_gateway_cert and new_gateway_cert is not None:
      certs[new_gateway_cert.gateway_id] = new_gateway_cert
      certs_pb[new_gateway_cert.gateway_id] = new_gateway_cert.SerializeToString()
   
   
   # add volume cert (as block 0)
   cert_block = cert_manifest.blocks.add()
   
   cert_block.block_id = volume_cert.volume_id 
   cert_block.block_version = volume_cert.volume_version
   cert_block.owner_id = volume_cert.owner_id
   cert_block.caps = 0
   cert_block.hash = hash_data( volume_cert.SerializeToString() )       # NOTE: covers volume signature as well
   
   # put blocks in order by ID 
   block_order = sorted( certs.keys() )
   
   # put the cert information into place...
   # gateways start at block 1
   for gateway_id in block_order:
      
      gateway_cert = certs[gateway_id]
      
      cert_block = cert_manifest.blocks.add()
      
      cert_block.block_id = gateway_id 
      cert_block.block_version = 0
      cert_block.owner_id = gateway_cert.owner_id
      cert_block.caps = gateway_cert.caps
      
   cert_manifest.size = len( certs.keys() )
   cert_manifest.signature = ""
   
   manifest_str = cert_manifest.SerializeToString()
   sig = sign_data( owner_privkey, manifest_str )
   
   cert_manifest.signature = base64.b64encode( sig )
   
   return cert_manifest.SerializeToString()

   
def load_gateway_type_aliases( config ):
    """
    Load the set of gateway type aliases.
    Returns a dict mapping type alias to type ID
    Returns empty dict if ~/.syndicate/gateway/types.conf does not exist.
    """

    types_path = conf.object_file_path( config, "gateway", "types.conf" )
    if os.path.exists( types_path ):
        # parse it 
        with open(types_path, "r") as f:
            type_lines = f.readlines()
            types_and_comments = [tl.strip() for tl in type_lines]
            
            # skip comments and empty lines
            types = filter( lambda t: len(t) > 0 and not t.startswith("#"), types_and_comments )
            
            # throw on invalid lines
            invalid_lines = filter( lambda t: t.count("=") != 1, types )
            if len(invalid_lines) > 0:
                raise Exception("Invalid type alias lines: %s" % (", ".join( ["'%s'" % il for il in invalid_lines] )))

        type_dict = {}  # type alias to type
        for t in types:
            
            parts = t.split("=")
            try:
                type_id = int(parts[1])
            except:
                raise Exception("Invalid type alias '%s'" % t)
            
            type_dict[parts[0]] = type_id
            
        return type_dict 
    
    else:
        return {}
            
            
class StubObject( object ):
   """
   Stub object class with just enough functionality to be compatible with 
   the MS's storagetypes.Object class.  This class includes extra information 
   for parsing and validating arguments that are derived from or relate to 
   object data.
   """
   def __init__(self, *args, **kw):
      pass
   
   @classmethod
   def Authenticate( cls, *args, **kw ):
      raise Exception("Called stub Authenticate method!  Looks like you have an import error somewhere.")
   
   @classmethod
   def Sign( cls, *args, **kw ):
      raise Exception("Called stub Sign method!  Looks like you have an import error somewhere.")
   
   @classmethod
   def parse_or_generate_private_key( cls, pkey_str, pkey_generate_args, key_size ):
      """
      Check a private key (pkey_str) and verify that it has the appopriate security 
      parameters.  If pkey_str is in pkey_generate_args (that is, pkey_str is a directive to generate a key pair),
      then generate a public/private key pair.
      Return the key pair.
      """
      import syndicate.ms.api as api
      
      if pkey_str in pkey_generate_args:
         # generate one
         pubkey_str, pkey_str = api.generate_key_pair( key_size )
         return pubkey_str, pkey_str
      
      else:
         # validate a given one
         try:
            pkey = CryptoKey.importKey( pkey_str )
         except Exception, e:
            log.exception(e)
            raise Exception("Failed to parse private key")
         
         # is it the right size?
         if pkey.size() != key_size - 1:
            raise Exception("Private key has %s bits; expected %s bits" % (pkey.size() + 1, key_size))
         
         return pkey.publickey().exportKey(), pkey_str
   
   
   @classmethod
   def parse_gateway_caps( cls, caps_str, lib ):
      """
      Interpret a bitwise OR of gateway caps as a string.
      """
      ret = 0
      
      aliases = {
         "ALL": "GATEWAY_CAP_READ_DATA|GATEWAY_CAP_WRITE_DATA|GATEWAY_CAP_READ_METADATA|GATEWAY_CAP_WRITE_METADATA|GATEWAY_CAP_COORDINATE",
         "NONE": 0,     # recommended for RGs
         "READWRITE_METADATA": "GATEWAY_CAP_READ_METADATA|GATEWAY_CAP_WRITE_METADATA",  # recommended for AGs
         "READWRITE": "GATEWAY_CAP_READ_DATA|GATEWAY_CAP_WRITE_DATA|GATEWAY_CAP_READ_METADATA|GATEWAY_CAP_WRITE_METADATA",
         "READONLY": "GATEWAY_CAP_READ_DATA|GATEWAY_CAP_READ_METADATA"
      }
      
      if aliases.has_key( caps_str ):
         caps_str = aliases[caps_str]
      
      if isinstance( caps_str, str ):
         flags = caps_str.split("|")
         ret = 0
         
         for flag in flags:
            value = getattr( msconfig, flag, 0 )
            if value == 0:
               raise Exception("Unknown Gateway capability '%s'" % flag)
            
            try:
               ret |= value
            except:
               raise Exception("Invalid value '%s'" % value)
      
      elif isinstance( caps_str, int ):
         ret = caps_str 
      
      else:
         raise Exception("Could not parse capabilities: '%s'" % caps_str )
         
      lib.caps = ret
      return ret, {}
   
   
   @classmethod
   def parse_email( cls, email, lib ):
      """
      Make sure email is an email address.
      Store it as the 'email' key.
      """
   
      if not email_regex.match(email):
         raise Exception("Not an email address: '%s'" % email)
      else:
         if lib is not None:
            lib.email = email
            
         return email, {"email": email}
      
      return email, {}
   
   
   @classmethod
   def parse_volume_name( cls, volume_name, lib ):
      """
      Consume volume name
      """
      
      lib.volume_name = volume_name 
      return volume_name, {"volume_name": volume_name}
   

   # Map an argument name to a function that parses and validates it.
   arg_parsers = {}
   
   @classmethod
   def ParseArgs( cls, config, method_name, argspec, args, kw, lib ):
      """
      Insert arguments and keywords from commandline-given arguments.
      Return the new args and kw, as well as a dict of extra information 
      generated by the arg parser that the caller might want to know.
      This method walks the arg_parsers class method.
      """
      extras_all = {}
      
      parsed = []
      
      #log.info("argspec: args=%s, defaults=%s" % (argspec.args, argspec.defaults))
      #log.info("args = %s" % [str(s) for s in args])
      
      # parse args in order
      for i in xrange(0, len(argspec.args)):
         argname = argspec.args[i]
         
         log.debug("parse argument '%s'" % argname)
         
         arg_func = cls.arg_parsers.get( argname, None )
         if arg_func != None:
            args, kw, extras = cls.ReplaceArg( argspec, argname, arg_func, args, kw, lib )
            extras_all.update( extras )
         
         parsed.append( argname )
      
      # parse the keyword arguments, in lexigraphical order
      unparsed = list( set(cls.arg_parsers.keys()) - set(parsed) )
      unparsed.sort()
      
      for argname in unparsed:
         arg_func = cls.arg_parsers[argname]
         args, kw, extras = cls.ReplaceArg( argspec, argname, arg_func, args, kw, lib )
         extras_all.update( extras )
      
      # process arguments before the call
      args, kw, extras_all = cls.PreProcessArgs( config, method_name, args, kw, extras, lib )
      
      return args, kw, extras_all
   
   
   @classmethod 
   def PreProcessArgs( config, method_name, args, kw, extras, lib ):
      """
      Do processing on the parsed args, before calling the method.
      
      Return (args, keywords, extras)
      
      Subclasses should override this.
      """
      
      return args, kw, extras
   
   
   @classmethod 
   def replace_kw( cls, arg_name, arg_value_func, value, lib ):
      """
      Replace a single keyword argument.
      """
      ret = arg_value_func( cls, value, lib )
      if ret is None:
         raise Exception("Got None for parsing %s" % (arg_name))
      
      lret = 0
      try:
         lret = len(ret)
      except:
         raise Exception("Got non-iterable for parsing %s" % (arg_name))
      
      if len(ret) != 2:
         raise Exception("Invalid value from parsing %s" % (arg_name))
      
      return ret[0], ret[1]


   @classmethod
   def ReplaceArg( cls, argspec, arg_name, arg_value_func, args, kw, lib ):
      """
      Replace a positional or keyword argument named by arg_name by feeding 
      it through with arg_value_func (which takes the argument value as its
      only argument).
      Return the positional arguments, keyword arguments, and extra information
      generated by arg_value_func.
      """
      # find positional argument?
      args = list(args)
      extras = {}
      replaced = False
      
      # replace positional args
      for i in xrange(0, min(len(args), len(argspec.args))):
         if argspec.args[i] == arg_name:
            args[i], arg_extras = arg_value_func( cls, args[i], lib )
            
            log.debug("positional argument '%s' is now '%s'" % (arg_name, args[i]) )
            
            extras.update( arg_extras )
            
            replaced = True
      
      # replace keyword args
      if not replaced and argspec.defaults != None:
         
         # replace default args 
         for i in xrange(0, len(argspec.defaults)):
            
            if argspec.args[ len(argspec.args) - len(argspec.defaults) + i ] == arg_name:
               
               value = None
               if arg_name in kw.keys():
                  value = kw[arg_name]
               else:
                  value = argspec.defaults[i]
               
               ret = cls.replace_kw( arg_name, arg_value_func, value, lib )
               
               log.debug("defaulted keyword argument '%s' is now '%s'" % (arg_name, ret[0]))
            
               kw[arg_name], arg_extras = ret[0], ret[1]
               
               extras.update( arg_extras )
            
               replaced = True
      
      # find keyword argument?
      if not replaced and arg_name in kw.keys():
         ret = cls.replace_kw( arg_name, arg_value_func, kw[arg_name], lib )
         
         log.debug("keyword argument '%s' is now '%s'" % (arg_name, ret[0]) )
         
         kw[arg_name], arg_extras = ret[0], ret[1]
         
         extras.update( arg_extras )
         
         replaced = True
      
      return (args, kw, extras)      
         
   @classmethod
   def PostProcessResult( cls, extras, config, method_name, args, kw, result ):
      """
      Do post-call processing--takes the result and extra data generated 
      from pre-processing.
      
      Subclasses should override this method
      """
      
      pass
   

   
class SyndicateUser( StubObject ):
   
   arg_parsers = dict( StubObject.arg_parsers.items() + {
      
      # required 
      "email": (lambda cls, arg, lib: cls.parse_email(arg, lib)),
      
      # required for create
      "private_key": (lambda cls, arg, lib: cls.parse_or_generate_private_key(arg, lib)),
      
      # required for create, update
      "max_volumes": (lambda cls, arg, lib: cls.parse_max_volumes(arg, lib)),
      
      # required for create, update 
      "max_gateways": (lambda cls, arg, lib: cls.parse_max_gateways(arg, lib)),
      
      # required for create 
      "is_admin": (lambda cls, arg, lib: cls.parse_is_admin(arg, lib))
      
   }.items() )
   
   
   @classmethod
   def parse_or_generate_private_key( cls, private_key, lib ):
      """
      Check a private key and verify that it has the appropriate security 
      parameters.  Interpret AUTO as a command to generate and return one.
      
      private_key can be a literal PEM-encoded private key, the string "auto"
      (interpreted to mean "generate one for me"), or a path to a private key.
      
      Set lib.public_key to be the PEM-encoded public key.
      Set lib.private_key to be the PEM-encoded private key
      
      Return private key, extras.
      """
      import syndicate.ms.api as api
      
      extra = {}
      pubkey_pem = None 
      
      if private_key is None or private_key == "" or private_key.upper() == "AUTO":
          
         pubkey_pem, privkey_pem = api.generate_key_pair( OBJECT_KEY_SIZE )
         extra['public_key'] = pubkey_pem
         extra['private_key'] = privkey_pem
         
         public_key = pubkey_pem
         
         lib.public_key = pubkey_pem
         lib.private_key = privkey_pem
      
      else:
         # is this a key literal?
         try:
            privkey = CryptoKey.importKey( private_key )
            if not privkey.has_private():
                raise Exception("Not a private key")
            
            extra['public_key'] = privkey.exportKey()
            lib.private_key = private_key
            lib.public_key = extra['public_key']
            
            return private_key, extra
         
         except:
            # not a key literal
            pass
         
         # is this a path?
         try:
            privkey = storagelib.read_private_key( private_key )
         except:
            raise Exception("Failed to load %s" % private_key )
         
         privkey_pem = privkey.exportKey()
         pubkey_pem = privkey.public_key().exportKey()
         extra['private_key'] = privkey_pem
         extra['public_key'] = pubkey_pem
         lib.private_key = privkey_pem
         lib.public_key = pubkey_pem
         
      return privkey_pem, extra

   
   @classmethod 
   def parse_max_gateways( cls, max_gateways, lib ):
      """
      parse max gateways.
      set lib.max_gateways
      """
      try:
         lib.max_gateways = int(max_gateways)
      except:
         raise Exception("Invalid max_gateways '%s'" % max_gateways)
      
      return lib.max_gateways, {}
   
   
   @classmethod 
   def parse_max_volumes( cls, max_volumes, lib ):
      """
      parse max volumes 
      set lib.max_volumes
      """
      try:
         lib.max_volumes = int(max_volumes)
      except:
         raise Exception("Invalid max_volumes '%s'" % max_volumes)
      
      return lib.max_volumes, {}
   
   
   @classmethod 
   def parse_is_admin( cls, is_admin, lib ):
      """
      parse is_admin 
      set lib.is_admin 
      """
      try:
         lib.is_admin = bool(is_admin)
      except:
         raise Exception("Invalid is_admin '%s'" % is_admin)
      
      return lib.is_admin, {}
   
   
   @classmethod 
   def PreProcessArgs( cls, config, method_name, args, kw, extras, lib ):
      """
      Preprocess method arguments on User objects.
      
      For creating a user:
        Generate a user cert and sign it with the admin's key.
      """
      
      if method_name in ["read_user", "list_users"]:
         # good to go 
         
         if method_name == "list_users" and len(args) == 0:
             # empty query 
             args.append({})
             
         return args, kw, extras
     
      email = getattr(lib, "email", None)
      if email is None:
         raise Exception("No user email given")
      
      if method_name in ["delete_user"]:
         # good to go 
         extras['email'] = email
         return args, kw, extras
          
      existing_user_cert = load_user_cert( config, email )
      
      owner_id = None
      admin_id = None
      public_key = None
      private_key = None
      max_volumes = getattr(lib, "max_volumes", None)
      max_gateways = getattr(lib, "max_gateways", None)
      is_admin = getattr(lib, "is_admin", None)
   
      admin_email = None 
      admin_privkey = None 
      admin_id = None
      
      cert_privkey = None
      
      # do we have a cert on file already?
      if existing_user_cert is not None:
         
         # we can't change the public key once set; we can only reset 
         if hasattr(lib, "public_key") and lib.public_key != existing_user_cert.public_key and existing_user_cert.public_key != "unset":
            raise Exception("Cannot change public key once set.  Instead, reset the user account to do so.")
         
         admin_id = existing_user_cert.admin_id 
         public_key = existing_user_cert.public_key 
         owner_id = existing_user_cert.user_id 
         max_volumes = existing_user_cert.max_volumes
         max_gateways = existing_user_cert.max_gateways
         is_admin = existing_user_cert.is_admin
         
      else:
         
         # generate our required fields 
         if method_name == "create_user":
            
            owner_id = random.randint( 0, 2**63 - 1 )
            
            public_key = getattr(lib, "public_key", None )
            private_key = getattr(lib, "private_key", None )
        
            if is_admin is None:
               is_admin = False 
               
            if max_volumes is None:
               max_volumes = 10 
               
            if max_gateways is None:
               max_gateways = 10
         
         else:
            # need a cert for all other methods 
            raise Exception("No user cert on file for '%s'" % email)
      
      
      if method_name in ["create_user", "reset_account_credentials", "delete_user"]:
         
         # get the admin's ID and private key
         admin_email = config['username']
         admin_privkey = storagelib.load_private_key( config, "user", admin_email )
         admin_id = load_id( config, "user", admin_email )
         
         if admin_privkey is None:
            raise Exception("No admin private key found for '%s'" % admin_email )
         
         if admin_id is None:
            raise Exception("No admin ID found for '%s'" % admin_email )
         
         # admin will be signing this request 
         cert_privkey = admin_privkey
      
      else:
         
         # user will be signing this request 
         cert_privkey = storagelib.load_private_key( config, "user", email )
         if cert_privkey is None:
            raise Exception("No user private key found for '%s'" % email )
      
      user_cert = ms_pb2.ms_user_cert()
      
      user_cert.user_id = owner_id 
      user_cert.email = email 
      user_cert.public_key = public_key 
      user_cert.admin_id = admin_id 
      user_cert.is_admin = is_admin 
      user_cert.max_gateways = max_gateways
      user_cert.max_volumes = max_volumes
      user_cert.signature = "" 
      
      user_cert_nosig = user_cert.SerializeToString()
      
      sig = sign_data( cert_privkey, user_cert_nosig )
      
      user_cert.signature = base64.b64encode( sig )
      
      user_cert_bin = user_cert.SerializeToString()
      
      # generate arguments 
      new_args = [ email ]
      
      kw = {
         'user_cert_b64': base64.b64encode( user_cert_bin )
      }
      
      extras = {
         'email': email,
         'public_key': public_key,
         'private_key': private_key,
         'owner_id': owner_id,
         'user_cert': user_cert
      }
      
      if method_name == 'create_user':
         # keep the MS happy with a dummy key
         new_args.append( "" )
      
      else:
         new_args = args 
         
         if method_name == 'delete_user':
         
            # only the email is needed; the API request will have been signed by the user
            kw = {}
      
      return new_args, kw, extras
         
         
   @classmethod
   def PostProcessResult( cls, extras, config, method_name, args, kw, result ):
      """
      Post-process result of a method call.
      Update local database with results.
      """
      
      super( SyndicateUser, cls ).PostProcessResult( extras, config, method_name, args, kw, result )
      
      if method_name not in ["create_user", "delete_user", "reset_account_credentials"]:
         # nothing to do 
         return 
      
      # error?
      if type(result) == type(dict) and result.has_key('error'):
          return 
      
      if not extras.has_key('email'):
         raise Exception("BUG: user email not rememberd")
         
      # if we deleted the user, remove the private key as well
      if method_name == "delete_user":
         log.info("Erasing private key for %s" % extras['email'] )
            
         storagelib.erase_private_key( config, "user", extras['email'] )
         storagelib.erase_public_key( config, "user", extras['email'] )
         remove_id( config, "user", extras['email'] )
         remove_user_cert( config, extras['email'] )
      
      elif method_name in ["create_user"]:
         # created a user or activated an account.
         # either way, we got back the user's numeric ID, which we should remember 
         store_id( config, "user", extras['email'], result['owner_id'] )
         
         if extras["private_key"] is not None:
            storagelib.store_private_key( config, "user", extras["email"], extras["private_key"] )           
            
         store_user_cert( config, extras['user_cert'] )
         storagelib.store_public_key( config, "user", extras['email'], extras['public_key'])
          
      elif method_name in ['reset_account_credentials']:
          
         # blow away old key
         storagelib.erase_private_key( config, "user", extras['email'] )
         store_user_cert( config, extras['user_cert'] )
            
            

class Volume( StubObject ):
   
   arg_parsers = dict( StubObject.arg_parsers.items() + {
      
      # required
      "name":                   (lambda cls, arg, lib: cls.parse_volume_name(arg, lib)),
      
      # required on create
      "email":                  (lambda cls, arg, lib: cls.parse_email(arg, lib)),
                                    
      # required on create
      "description":            (lambda cls, arg, lib: cls.parse_volume_description(arg, lib)),
      
      # required on create
      "blocksize":              (lambda cls, arg, lib: cls.parse_volume_blocksize(arg, lib)),
      
      # optional; defaults to False
      "archive":                (lambda cls, arg, lib: cls.parse_volume_archive(arg, lib)),
      
      # optional; defaults to True 
      "private":                (lambda cls, arg, lib: cls.parse_volume_private(arg, lib)),
      
      # optional: default to False 
      "allow_anon":             (lambda cls, arg, lib: cls.parse_volume_allow_anon(arg, lib)),
      
      # optional: defaults to infinite 
      "file_quota":             (lambda cls, arg, lib: cls.parse_volume_file_quota(arg, lib))
      
   }.items() )
   
   @classmethod 
   def parse_volume_description( cls, description, lib ):
      lib.description = description
      return description, {}
   
   @classmethod 
   def parse_volume_blocksize( cls, blocksize, lib ):
      try:
         lib.blocksize = int(blocksize)
      except:
         raise Exception("Invalid blocksize '%s'" % blocksize)
      
      return lib.blocksize, {}
      
   @classmethod 
   def parse_volume_archive( cls, archive, lib ):
      try:
         lib.archive = bool(archive)
      except:
         raise Exception("Invalid archive flag '%s'" % archive)
      
      return lib.archive, {}
   
   @classmethod 
   def parse_volume_private( cls, private, lib ):
      try:
         lib.private = bool(private)
      except:
         raise Exception("Invalid private flag '%s'" % private)
      
      return lib.private, {}
   
   
   @classmethod 
   def parse_volume_allow_anon( cls, allow_anon, lib ):
      try:
         lib.allow_anon = bool(allow_anon)
      except:
         raise Exception("Invalid allow_anon flag '%s'" % allow_anon)
      
      return lib.allow_anon, {}
   
   
   @classmethod 
   def parse_volume_file_quota( cls, file_quota, lib ):
      try:
         lib.file_quota = int(file_quota)
      except:
         raise Exception("Invalid file quota '%s'" % file_quota )
      
      return lib.file_quota, {}
   
   
   @classmethod 
   def PreProcessArgs( cls, config, method_name, args, kw, extras, lib ):
      """
      Pre-method-call processing for a Volume
      
      When creating, updating, or deleting:
        generate a volume certificate and a new volume certificate bundle version vector.
      """
      
      if method_name in ["read_volume", "list_volumes", "list_public_volumes", "list_archive_volumes"]:
         
         # only accessing...
         if method_name == "list_volumes" and len(args) == 0:
             # empty query statement
             args.append( {} )
             
         # nothing to do 
         return args, kw, extras
      
      # otherwise, we're creating/updating/deleting.
      # we'll need the volume name
      if not hasattr(lib, "volume_name"):
         raise Exception("Missing volume name")
      
      volume_name = lib.volume_name
      
      existing_volume_cert = load_volume_cert( config, volume_name )
      if existing_volume_cert is None:
          
         # we need this, unless we're creating
         if method_name != "create_volume":
            raise Exception("No volume cert on file for '%s'" % volume_name )
         
      # to be looked up...
      owner_id = None 
      volume_id = None
      volume_version = None 
      volume_public_key = None 
      owner_privkey = None
      
      blocksize = getattr(lib, "blocksize", None)
      owner_username = getattr(lib, "email", None)
      description = getattr(lib, "description", None)
      archive = getattr(lib, "archive", False)
      private = getattr(lib, "private", True)
      allow_anon = getattr(lib, "allow_anon", False)
      file_quota = getattr(lib, "file_quota", None)
      
      if existing_volume_cert is not None:
         
         if blocksize is None:
            blocksize = existing_volume_cert.blocksize 
         
         if owner_username is None:
            owner_id = existing_volume_cert.owner_id
            owner_username = find_by_id( config, "user", owner_id )
            if owner_username is None:
               
               # no such user 
               raise Exception("No user identified as the volume owner (cert indicates ID %s)" % owner_id)
            
         if description is None:
            description = existing_volume_cert.description 
         
         if archive is None:
            archive = existing_volume_cert.archive 
            
         if allow_anon is None:
            allow_anon = existing_volume_cert.allow_anon 
         
         if file_quota is None:
            file_quota = existing_volume_cert.file_quota 
         
         # load dependent values
         volume_id = existing_volume_cert.volume_id
         volume_version = existing_volume_cert.volume_version 
         volume_public_key = existing_volume_cert.volume_public_key
         
         if owner_id is None:
            owner_id = load_id( config, "user", owner_username )
         
      else:
         
         # set defaults 
         if archive is None:
            archive = False 
            
         if allow_anon is None:
            allow_anon = False 
         
         if file_quota is None:
            file_quota = False 
            
         if description is None:
            now_sec, now_nsec = clock_gettime()
            description = "A volume created at %s.%s" % (now_sec, now_nsec)
            
         # enforce requirements...
         if blocksize is None:
            raise Exception("No blocksize given.  A good value is 61440 (60Kb)")
         
         if owner_username is None:
            raise Exception("No user identified as the volume owner")
         
         # load dependent values
         volume_id = random.randint( 0, 2**63 - 1 )
         volume_version = 0
         volume_public_key = storagelib.load_public_key( config, "user", owner_username ).exportKey()
         owner_id = load_id( config, "user", owner_username )
         
         if volume_public_key is None:
            raise Exception("No public key on file for user '%s'" % owner_username)
         
         if owner_id is None:
            raise Exception("No ID found for user '%s'" % owner_username)
         
         
      owner_privkey = storagelib.load_private_key( config, "user", owner_username )
      if owner_privkey is None:
         raise Exception("No private key found for user '%s'" % owner_username)
      
      volume_cert = ms_pb2.ms_volume_metadata()
      volume_cert.owner_id = owner_id 
      volume_cert.owner_email = owner_username
      volume_cert.volume_id = volume_id
      volume_cert.volume_version = volume_version + 1
      volume_cert.name = volume_name
      volume_cert.description = description
      volume_cert.volume_public_key = volume_public_key
      volume_cert.archive = archive 
      volume_cert.private = private 
      volume_cert.allow_anon = allow_anon 
      volume_cert.file_quota = file_quota
      volume_cert.blocksize = blocksize
      volume_cert.signature = ""
      
      # sign the cert, but not the root inode.
      volume_cert_bin = volume_cert.SerializeToString()
      volume_cert_sig = sign_data( owner_privkey, volume_cert_bin )
      
      volume_cert.signature = base64.b64encode( volume_cert_sig )
      
      volume_cert_bundle_str = make_volume_cert_bundle( config, owner_username, volume_name, new_volume_cert=volume_cert, volume_id=volume_id )
      
      # add and sign the (initial) root separately, if we're creating the volume
      if method_name == "create_volume":
         make_volume_root( volume_cert )
        
         root_str = volume_cert.root.SerializeToString()
         root_sig = sign_data( owner_privkey, root_str )
        
         volume_cert.root.signature = base64.b64encode( root_sig )
         
         volume_cert_bin = volume_cert.SerializeToString()
         volume_cert.ClearField("root")
      
      else:
         volume_cert_bin = volume_cert.SerializeToString()
      
      # construct the actual keyword arguments
      args = []
      kwargs = {
         'volume_cert_b64': base64.b64encode( volume_cert_bin ),
         'cert_bundle_b64': base64.b64encode( volume_cert_bundle_str )
      }
      
      extras = {
         "volume_cert": volume_cert,
         "volume_name": volume_name,
         "volume_id": volume_id
      }
      
      if method_name == 'update_volume':
         args = [volume_id]
      
      elif method_name == 'delete_volume':
         args = [volume_id]
         kwargs = {}
         
      return args, kwargs, extras
            
      
   
   @classmethod
   def PostProcessResult( cls, extras, config, method_name, args, kw, result ):
      # process keys 
      super( Volume, cls ).PostProcessResult( extras, config, method_name, args, kw, result )
      
      if method_name not in ["create_volume", "update_volume", "delete_volume"]:
         # nothing to do 
         return 
      
      if not result.has_key('error'):
         
         if method_name in ["create_volume", "update_volume"]:
            
            if not extras.has_key('volume_name'):
               raise Exception("BUG: no volume_name remembered")
            
            # store volume cert
            rc = store_volume_cert( config, extras['volume_cert'] )
            if not rc:
               raise Exception("Failed to store volume certificate for '%s'" % extras['volume_name'])
         
            rc = store_id( config, "volume", extras['volume_name'], extras['volume_id'])
            if not rc: 
               raise Exception("Failed to store volume ID %s for '%s'" % (extras['volume_id'], extras['volume_name']))
            
            rc = link_id_cert( config, "volume", extras['volume_id'], extras['volume_name'])
            if not rc:
               raise Exception("Failed to link volume id %s to cert for '%s'" % (extras['volume_id'], extras['volume_name']))
            
               
         # delete public key?
         if method_name == "delete_volume":
            
            volume_name = extras.get("volume_name", None )
            if volume_name == None:
               log.error("Could not determine name of Volume.  You will need to manaully delete its public key from your Syndicate key directory.")
            
            else:
               
               remove_volume_cert( config, volume_name )
               remove_id( config, "volume", volume_name )
               unlink_id_cert( config, "volume", extras['volume_id'] )
               
            

class Gateway( StubObject ):
   
   # NOTE: these are all keyword arguments
   arg_parsers = dict( StubObject.arg_parsers.items() + {
      
      # required
      "name":                   (lambda cls, arg, lib: cls.parse_gateway_name(arg, lib)),
      
      # required 
      "email":                  (lambda cls, arg, lib: cls.parse_email(arg, lib)),
      
      # required 
      "volume":                 (lambda cls, arg, lib: cls.parse_volume_name(arg, lib)),
      
      # required
      "type":                   (lambda cls, arg, lib: cls.parse_gateway_type(arg, lib)),
      
      # required
      "caps":                   (lambda cls, arg, lib: cls.parse_gateway_caps(arg, lib)),
      
      # optional; can be automatically filled in 
      "host":                   (lambda cls, arg, lib: cls.parse_gateway_host(arg, lib)),
      
      # required 
      "port":                   (lambda cls, arg, lib: cls.parse_gateway_port( arg, lib )),
      
      # optional
      "driver":                 (lambda cls, arg, lib: cls.parse_gateway_driver( arg, lib )),
      
      # optional; can be filled in automatically 
      "expires":                (lambda cls, arg, lib: cls.parse_gateway_cert_expires( arg, lib )),
      
      # can be the string "auto", or a PEM-encoded 4096-bit RSA key
      "public_key":             (lambda cls, arg, lib: cls.parse_or_generate_gateway_public_key(arg, lib)),
     
      # optional; used for development/debugging 
      "cert_version":           (lambda cls, arg, lib: cls.parse_gateway_cert_version( arg, lib ))
   }.items() )
   
   
   @classmethod
   def parse_gateway_name( cls, gateway_name, lib ):
      """
      Make usre gateway_name is a gateway name.
      Return (gateway name, dict with "name" set)
      """
      
      # needed for parse_gateway_driver
      if lib is not None:
         lib.name = gateway_name
      
      return gateway_name, {"name": gateway_name}
   
      
   @classmethod
   def parse_gateway_type( cls, type_str, lib ):
      """
      Parse gateway type ID.
      Return (type, empty dict)
      """
      
      gtype = None 
      
      try:
         # non-default type--treat as int
         gtype = int(type_str)
      except Exception, e:
         pass
      
      if gtype is not None:
         # needed for parse_gateway_config
         if lib is not None:
            lib.gateway_type = gtype
            
         return (gtype, {})
      raise Exception("Unknown Gateway type '%s'" % type_str)
   
   
   @classmethod 
   def parse_gateway_host( cls, gateway_host, lib ):
      """
      Store the host to lib.host.
      """
      lib.host = gateway_host
      return gateway_host, {}
   
   
   @classmethod 
   def parse_gateway_port( cls, gateway_port, lib ):
      """
      Store the integer port to lib.port 
      """
      try:
         lib.port = int(gateway_port)
      except Exception, e:
         raise Exception("Invalid gateway port '%s'" % gateway_port )
      
      return (lib.port, {})
   
   
   @classmethod 
   def parse_gateway_cert_expires( cls, expires, lib ):
      """
      Parse an expiry time.
      Format is in years (yr), days (d), hours (h), minutes (m), or seconds (s)
      i.e. 1yr, 365d, 24h, 30m, 100s
      Returns (number in seconds, {})
      """
      
      exp_secs = 0
      
      try:
         if expires.endswith("yr"):
            exp_secs = int( int( expires.strip("yr") ) * (60 * 60 * 24 * 365.25) )
         
         elif expires.endswith("d"):
            exp_secs = int( expires.strip("d") ) * (60 * 60 * 24)
         
         elif expires.endswith("h"):
            exp_secs = int( expires.strip("h") ) * (60 * 60)
         
         elif expires.endswith("m"):
            exp_secs = int( expires.strip("m") ) * 60 
         
         elif expires.endswith("s"):
            exp_secs = int( expires.strip("s") )
         
         else:
            raise Exception("Unrecognized time units")
      
         lib.expires = exp_secs 
         return (lib.expires, {})
      
      except Exception, e:
         raise Exception("Unable to parse '%s'.  Expect units of yr, d, h, m, or s")
      
   
   @classmethod
   def parse_or_generate_gateway_public_key( cls, gateway_public_key, lib ):
      """
      Load or generate a gateway public key.  Preserve the private key 
      as extra data if we generate one.
      
      Sets lib.gateway_public_key_str, lib.gateway_private_key_str if we generate.
      Otherwise, sets only lib.gateway_public_key_str
      
      Return (public key string, dict with 'gateway_private_key' set to the private key)
      """
      
      pubkey_str = ""
      privkey_str = None
      
      if gateway_public_key.lower() == "auto":
         # generate a key pair 
         pubkey_str, privkey_str = cls.parse_or_generate_private_key( "auto", ["auto"], OBJECT_KEY_SIZE )
         
      else:
         # validate a given one
         try:
            pubkey = CryptoKey.importKey( gateway_public_key )
         except Exception, e:
            log.exception(e)
            raise Exception("Failed to load public key")
         
         # is it the right size?
         if pubkey.size() != OBJECT_KEY_SIZE - 1:
            raise Exception("Public key has %s bits; expected %s bits" % (pubkey.size() + 1, OBJECT_KEY_SIZE))
         
         pubkey_str = gateway_public_key
      
      if privkey_str is not None:
         extra = {'gateway_private_key': privkey_str}      # pass along to store it on successful call
         lib.gateway_private_key_str = privkey_str
         
      lib.gateway_public_key_str = pubkey_str
      
      return pubkey_str, extra
   
   
   @classmethod 
   def parse_gateway_driver( cls, driver_path, lib ):
      """
      Store the driver path to lib.driver_path 
      """
      lib.driver_path = driver_path
      return driver_path, {}
   
  
   @classmethod 
   def parse_gateway_cert_version( cls, cert_version, lib ):
      """
      Development/debugging option: set the cert version directly.
      """
      lib.cert_version = int( cert_version )
      return lib.cert_version, {}


   @classmethod 
   def load_gateway_driver( cls, config, gateway_name, gateway_driver_path, privkey_pem ):
      """
      Parse the gateway driver.
      
      Called during argument postprocessing
      
      Returns a JSON string
      """
      
      # load the key 
      privkey = storagelib.load_private_key( config, "gateway", gateway_name )
      if privkey is None:
         raise Exception("No private key found for gateway '%s'" % gateway_name )
         
      privkey_pem = privkey.exportKey()
      
      driver = load_driver( gateway_driver_path, privkey_pem )
      if driver is None:
         raise Exception("Failed to load driver '%s'" % gateway_driver_path )
      
      # serialize...
      driver_json = None
      try:
         driver_json = json_stable_serialize( driver )
         
      except Exception, e:
         log.error("Failed to serialize '%s' to JSON" % gateway_driver_path )
         raise e
      
      return str(driver_json)
   
   
   @classmethod 
   def PreProcessArgs( cls, config, method_name, args, kw, extras, lib ):
      """
      Post-parsing processing: generate a gateway certificate as the sole argument, if we're creating, updating, or deleting
      
      If we're creating a gateway (something only the volume admin can do), then put a new volume cert bundle as well.
      
      Return new args, kw, extra
      """
      
      if method_name in ["read_gateway", "list_gateways"]:
         
         # good to go 
         if method_name == "list_gateways":
             if len(args) == 0:
                # empty query 
                args.append({})

             elif len(args) > 1:
                # invalid--must be a dict 
                raise Exception("Invalid query argument '%s': multiple aruments" % args)

             elif type(args[0]) != dict:
                raise Exception("Invalid query arguments '%s': not a dict" % args)
             
         return args, kw, extras
      
      # otherwise, we're creating/updating/deleting
      if not hasattr(lib, "name"):
         raise Exception("Missing gateway name")
      
      gateway_name = lib.name
      
      # see if there is already a cert on file 
      existing_gateway_cert = load_gateway_cert( config, lib.name )
      
      # sanity check
      if existing_gateway_cert is not None and method_name == "create_gateway":
         raise Exception("Certificate already exists for '%s'.  If this is an error, remove it from '%s'" % (gateway_name, conf.object_file_path(config, "gateway", gateway_name) + ".cert"))
      
      elif existing_gateway_cert is None and method_name == "update_gateway":
         raise Exception("No certificate on file for '%s'." % (gateway_name))
     
      # see if we own the volume in question.
      # the volume needs to exist either way.
      volume_id = None 
      volume_name = None 
      
      existing_volume_cert = None 
      if hasattr( lib, "volume_name" ):
         volume_name = lib.volume_name
         existing_volume_cert = load_volume_cert( config, volume_name )
         if existing_volume_cert is not None:
            volume_id = existing_volume_cert.volume_id
      
      elif existing_gateway_cert is not None:
         # get volume cert, and then volume name 
         volume_id = existing_gateway_cert.volume_id 
         volume_name = find_by_id( config, "volume", volume_id )
         if volume_name is not None:
            existing_volume_cert = load_volume_cert( config, volume_name )
      
      # given volume?
      if existing_volume_cert is None and hasattr( lib, "volume_id" ):
         volume_id = lib.volume_id
         volume_name = find_by_id( config, "volume", volume_id )
         if volume_name is not None:
            existing_volume_cert = load_volume_cert( config, volume_name )
      
      if existing_volume_cert is None:
         raise Exception("No volume cert on file for '%s (%s)'.  This volume must exist before you can create a gateway in it." % (volume_name, volume_id))
      
      gateway_name = lib.name
      gateway_type = getattr(lib, "gateway_type", None)
      owner_username = getattr(lib, "email", None)
      gateway_name = getattr(lib, "name", None)
      host = getattr(lib, "host", None )
      port = getattr(lib, "port", None )
      public_key = getattr(lib, "gateway_public_key_str", None)
      private_key = getattr(lib, "gateway_private_key_str", None)
      cert_expires = getattr(lib, "cert_expires", None)
      caps = getattr(lib, "caps", None)
      driver_path = getattr(lib, "driver_path", None )
      driver_json = None
      cert_version = getattr(lib, "cert_version", None )

      owner_id = None 
      gateway_id = None
     
      # sanity check 
      missing = []
      if gateway_type is None:
         if existing_gateway_cert is not None:
            gateway_type = existing_gateway_cert.gateway_type
         
         else:
            gateway_type = 0
      
      else:
         # could be a type alias 
         if type(gateway_type) not in [int, long]:
            type_aliases = load_gateway_type_aliases( config )
            
            if gateway_type in type_aliases.keys():
                gateway_type = type_aliases[ gateway_type ]
            
            else:
                raise Exception("Unaliased gateway type '%s'" % gateway_type)
      
      # sanity check...
      if method_name == "create_gateway":
          if private_key is None:
              missing.append("gateway_private_key")
      
      if owner_username is None:
         if existing_gateway_cert is not None:
            owner_id = existing_gateway_cert.owner_id 
            
            # find the associated username, so we can get the public key
            owner_username = find_by_id( config, "user", owner_id )
            if owner_username is None:
               missing.append("email")
         
         else:
            missing.append("email")
      
      if public_key is None:
         if existing_gateway_cert is not None:
            public_key = existing_gateway_cert.public_key 
         
         else:
            missing.append("public_key")
      
      if volume_id is None:
         if existing_gateway_cert is not None:
            volume_id = existing_gateway_cert.volume_id 
         
         elif volume_name is not None:
            # load from the database, if we can 
            volume_id = load_id( config, "volume", volume_name )
            if volume_id is None:
               missing.append("volume_name_or_id")
               
         else:
            missing.append("volume_name_or_id")
            
      
      if volume_name is None:
         # we have the ID, so look up the name 
         volume_name = find_by_id( config, "volume", volume_id )
         if volume_name is None:
            missing.append("volume_name_or_id")
            
      if port is None:
         if existing_gateway_cert is not None:
            port = existing_gateway_cert.port 
         
         else:
            # default 
            port = GATEWAY_DEFAULT_PORT
         
      if len(missing) > 0:
         raise Exception("Missing the following required keyword arguments: %s" % ", ".join(missing) )
      
      
      # find or create gateway ID
   
      if existing_gateway_cert is not None:
         gateway_id = existing_gateway_cert.gateway_id 
   
      else:
         # load from database, if we can 
         gateway_id = load_id( config, "gateway", gateway_name )
         
         if gateway_id is None:
            
            # no ID on file
            if method_name == "create_gateway":
               
               # ...because we still need to generate one 
               gateway_id = random.randint( 0, 2**63 - 1 )
               
            else:
               raise Exception("Could not determine gateway ID for gateway '%s'" % gateway_name)
   
      
      # load driver
      if driver_path is not None:
         try:
            driver_json = Gateway.load_gateway_driver( config, gateway_name, driver_path, private_key )
         except Exception, e:
            traceback.print_exc()
            raise Exception("Unable to load driver for '%s' from '%s'" % (gateway_name, driver_path) )
      
      
      # driver hash 
      driver_hash = None 
      if driver_json is not None:
         driver_hash = hash_data( driver_json )
      else:
         driver_hash = hash_data( "" )
      
      # load host
      if host is None:
         if existing_gateway_cert is not None:
            host = existing_gateway_cert.host
         else:
            
            # get from socket 
            host = socket.gethostname()
      
      # load caps 
      cur_caps = 0
      if existing_gateway_cert is not None:
          cur_caps = existing_gateway_cert.caps 
          
      if caps is None:
         if existing_gateway_cert is not None:
            caps = cur_caps
         else:
            caps = 0
      
      # load user ID
      if owner_id is None:
         owner_id = load_id( config, "user", owner_username )
         if owner_id is None:
            raise Exception("Unable to determine user ID of '%s'" % owner_username)
      
      # load user private key, so we can sign the cert and make a new volume cert bundle
      user_privkey = storagelib.load_private_key( config, "user", owner_username )
      
      if user_privkey is None:
         raise Exception("No private key found for user '%s'" % owner_username)
      
      # load cert expires (1 year default expiry)
      if cert_expires is None:
         cert_expires = 60 * 60 * 24 * 365
      
      # cert version...
      if cert_version is None:
          if existing_gateway_cert is not None:
             
             # must increment
             cert_version = existing_gateway_cert.version + 1
          else:
             
             # first version of this gateway
             cert_version = 1
         
      now_sec, _ = clock_gettime()
      
      # generate a cert and sign it with the user's private key and the volume owner's private key
      gateway_cert = ms_pb2.ms_gateway_cert()
      
      gateway_cert.gateway_type = gateway_type 
      gateway_cert.gateway_id = gateway_id 
      gateway_cert.owner_id = owner_id 
      gateway_cert.name = gateway_name 
      gateway_cert.host = host 
      gateway_cert.port = port 
      gateway_cert.public_key = public_key 
      gateway_cert.version = cert_version
      gateway_cert.cert_expires = cert_expires + now_sec
      gateway_cert.caps = caps 
      gateway_cert.volume_id = volume_id
      gateway_cert.driver_hash = driver_hash 
      gateway_cert.signature = ""
      
      # sign with user's private key 
      gateway_cert_str = gateway_cert.SerializeToString()
      sig = sign_data( user_privkey, gateway_cert_str )
      
      gateway_cert.signature = base64.b64encode( sig )
      gateway_cert_str = gateway_cert.SerializeToString()
      
      volume_cert_bundle_str = None 
      need_volume_cert_bundle = False
      
      if method_name in ["update_gateway"] and (cur_caps | caps) != cur_caps:
         # we will need a cert bundle if we're expanding the gateway's capabilities 
         need_volume_cert_bundle = True
         
      elif method_name in ["create_gateway", "delete_gateway"]:
         need_volume_cert_bundle = True
      
      elif owner_id == existing_volume_cert.owner_id and existing_gateway_cert is not None and owner_id == existing_gateway_cert.owner_id:
         # if the admin or volume owner is updating a gateway besides their own,
         # then they need to update the bundle as well (so the remote gateway 
         # will detect the change)
         need_volume_cert_bundle = True
      
      # make cert bundle
      if need_volume_cert_bundle:
         
         # we must also be the volume owner.
         if owner_id != existing_volume_cert.owner_id:
             raise Exception("User '%s' does not own volume '%s'" % (owner_username, volume_name))
         
         # generate a certificate bundle.
         volume_cert_bundle_str = make_volume_cert_bundle( config, owner_username, volume_name, new_gateway_cert=gateway_cert )
         if volume_cert_bundle_str is None:
             raise Exception("Failed to generate volume cert bundle for Volume '%s' (Gateway '%s')" % (volume_name, gateway_name))
      
      # generate the actual keyword arguments for the API call
      args = []
      kw = {
         'gateway_cert_b64': base64.b64encode( gateway_cert_str )
      }

      if driver_json is not None:
          kw['driver_text'] = driver_json
      
      if volume_cert_bundle_str is not None:
          kw['cert_bundle_b64'] = base64.b64encode( volume_cert_bundle_str )
      
      # if we're updating/deleting, we expect an ID
      if method_name in ['update_gateway', 'delete_gateway']:
         args = [gateway_id]
      
      # pass this along to our result post-processor
      extras['gateway_cert'] = gateway_cert 
      extras['gateway_id'] = gateway_id
      extras['username'] = owner_username
      extras['volume_name'] = volume_name
      extras['name'] = gateway_name 
      extras['gateway_private_key'] = private_key
      
      return args, kw, extras
      
   
   @classmethod
   def PostProcessResult( cls, extras, config, method_name, args, kw, result ):
      """
      Process extra information generated by parsing arguments.
      Called after the RPC completes (result is the returned data)
      """
      
      # process keys
      super( Gateway, cls ).PostProcessResult( extras, config, method_name, args, kw, result )
      
      if method_name not in ["create_gateway", "update_gateway", "delete_gateway", "remove_user_from_volume"]:
          # nothing to store
          return 
      
      if not result.has_key('error'):
         
         # store private key key, if we have it
         if method_name == "create_gateway":
        
            # remember our private key 
            gateway_name = extras.get("name", None)
            if gateway_name is None:
                raise Exception("BUG: gateway name not stored")
            
            # store it
            rc = storagelib.store_private_key( config, "gateway", gateway_name, extras['gateway_private_key'] )
            if not rc:
                raise Exception("Failed to store private key to for '%s'.  Text: '%s'" % (gateway_name, extras['gateway_private_key']))
               
               
         if method_name in ["create_gateway", "update_gateway"]:
        
            # remember our cert 
            gateway_name = extras.get("name", None)
            if gateway_name is None:
                raise Exception("BUG: gateway name not stored")
            
            gateway_cert = extras.get('gateway_cert', None)
            if gateway_cert is None:
                raise Exception("BUG: gateway cert not stored")
            
            gateway_id = extras.get('gateway_id', None)
            if gateway_id is None:
                raise Exception("BUG: gateway ID not stored")
            
            # store it 
            store_gateway_cert( config, gateway_cert )
            
            # remember our ID 
            store_id( config, "gateway", gateway_name, gateway_id )
            
         else:
            raise Exception("BUG: gateway cert not stored")
               
         
         # erase private key if deleted
         if method_name == "delete_gateway":
            gateway_name = extras.get("name", None)
            if gateway_name == None:
               log.error("Failed to determine the name of the gateway.  You will need to remove the gateway's private key manually")
            
            else:
               remove_gateway_cert( config, gateway_name )
               storagelib.erase_private_key( config, "gateway", gateway_name )
               remove_id( config, "gateway", gateway_name )


object_classes = [SyndicateUser, Volume, Gateway]