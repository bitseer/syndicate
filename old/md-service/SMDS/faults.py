#!/usr/bin/python

import xmlrpclib

class MDException( xmlrpclib.Fault ):
  def __init__(self, faultCode, faultString, extra = None):
    if extra:
      faultString += ": " + extra
    xmlrpclib.Fault.__init__(self, faultCode, faultString)


class MDMethodNotFound( MDException ):
  def __init__(self, method):
    faultString = "No such method: '%s'" % method
    super(MDMethodNotFound, self).__init__( 601, faultString, None )


class MDMethodFailed( MDException ):
  def __init__(self, method, exc ):
    faultString = "Method '%s' failed; problem = '%s'" % (method, exc)
    super(MDMethodFailed, self).__init__( 602, faultString, None )


class MDInvalidArgument( MDException ):
  def __init__(self, faultString, method=None ):
    if method:
      super(MDInvalidArgument, self).__init__(603, "%s: %s" % (method, faultString), None )
    else:
      super(MDInvalidArgument, self).__init__( 603, faultString, None )


class MDDBError( MDException ):
  def __init__(self, faultString ):
    super(MDDBError, self).__init__( 604, faultString, None )
    
    
class MDAuthenticationFailure( MDException ):
  def __init__(self, faultString ):
    super(MDAuthenticationFailure, self).__init__( 605, faultString, None )
    
    
class MDInvalidArgumentCount( MDException ):
  def __init__(self, num_given, min_allowed, max_allowed ):
    super(MDInvalidArgumentCount, self).__init__( 606, "%s arguments given, but only between %s and %s are allowed" % (num_given, min_allowed, max_allowed), None )
  
  
class MDObjectNotFound( MDException ):
   def __init__(self, object_name, object_details=None ):
      super(MDObjectNotFound, self).__init__(607, "Object '%s' could not be read (details: %s)" % (object_name, str(object_details)), None)
      
      
class MDInternalError( MDException ):
   def __init__(self, faultString ):
      super(MDInternalError, self).__init__(608, faultString, None)
   

class MDResourceExceeded( MDException ):
   def __init__(self, consumer, resource ):
      super(MDResourceExceeded, self).__init__(609, "Object '%s' attempted to create too many '%s' objects" % (consumer, resource), None)
      

class MDUnauthorized( MDException ):
   def __init__(self, faultString ):
      super(MDUnauthorized, self).__init__(610, faultString, None)
      
      
class MDMetadataServerError( MDException ):
   def __init__(self, faultString ):
      super(MDMetadataServerError, self).__init__(611, faultString, None)