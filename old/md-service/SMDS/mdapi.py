#!/usr/bin/python

import SMDS.xmlrpc_ssl
import SMDS.logger as logger
import SMDS.Methods
from SMDS.faults import *
from SMDS.config import Config
from SMDS.postgres import PostgreSQL
from SMDS.API.CDN import CDN
from SMDS.API.CDN.PL_CoBlitz import PL_CoBlitz
from SMDS.user import *

import pgdb
import string
import xmlrpclib
import subprocess
import os
import random
import traceback

"""
Below code was copied from PLCAPI, August 2011.
Courtesy of PlanetLab.  Copyright Princeton Board of Trustees.
"""

invalid_xml_ascii = map(chr, range(0x0, 0x8) + [0xB, 0xC] + range(0xE, 0x1F))
xml_escape_table = string.maketrans("".join(invalid_xml_ascii), "?" * len(invalid_xml_ascii))

def xmlrpclib_escape(s, replace = string.replace):
    """
    xmlrpclib does not handle invalid 7-bit control characters. This
    function augments xmlrpclib.escape, which by default only replaces
    '&', '<', and '>' with entities.
    """

    # This is the standard xmlrpclib.escape function
    s = replace(s, "&", "&amp;")
    s = replace(s, "<", "&lt;")
    s = replace(s, ">", "&gt;",)

    # Replace invalid 7-bit control characters with '?'
    return s.translate(xml_escape_table)

def xmlrpclib_dump(self, value, write):
    """
    xmlrpclib cannot marshal instances of subclasses of built-in
    types. This function overrides xmlrpclib.Marshaller.__dump so that
    any value that is an instance of one of its acceptable types is
    marshalled as that type.

    xmlrpclib also cannot handle invalid 7-bit control characters. See
    above.
    """

    # Use our escape function
    args = [self, value, write]
    if isinstance(value, (str, unicode)):
        args.append(xmlrpclib_escape)

    try:
        # Try for an exact match first
        f = self.dispatch[type(value)]
    except KeyError:
        # Try for an isinstance() match
        for Type, f in self.dispatch.iteritems():
            if isinstance(value, Type):
                f(*args)
                return
        raise TypeError, "cannot marshal %s objects" % type(value)
    else:
        f(*args)

# You can't hide from me!
xmlrpclib.Marshaller._Marshaller__dump = xmlrpclib_dump

# SOAP support is optional
try:
    import SOAPpy
    from SOAPpy.Parser import parseSOAPRPC
    from SOAPpy.Types import faultType
    from SOAPpy.NS import NS
    from SOAPpy.SOAPBuilder import buildSOAP
except ImportError:
    SOAPpy = None


def import_deep(name):
    mod = __import__(name)
    components = name.split('.')
    for comp in components[1:]:
        mod = getattr(mod, comp)
    return mod

class MDAPI:

    # flat list of method names
    all_methods = SMDS.Methods.methods
    
    DEFAULT_MAX_MDSERVERS = 10
    DEFAULT_MAX_CONTENTS = 1

    def __init__(self, config = "/etc/syndicate/syndicate-metadata-service.conf", encoding = "utf-8"):
        self.config = Config(config)
        
        CDN_inst = None
        try:
           cdn_interface = __import__("SMDS.API.CDN." + self.config.MD_CDN_INTERFACE, fromlist=["SMDS"])
           logger.info("Using CDN driver %s" % cdn_interface.__name__)
           CDN_inst = cdn_interface.CDNDriver()
        except Exception, e:
           traceback.print_exc()
           CDN_inst = PL_CoBlitz()
        
        self.encoding = encoding

        # Better just be documenting the API
        if config is None:
            return

        # Initialize database connection
        self.db = PostgreSQL(self)
        
        # keep the CDN interface around, and tell it to set up
        self.cdn = CDN_inst
        self.cdn.setup( self )
        
        # keep a maintenance auth structure around
        self.maint_auth = {'AuthMethod':'password','Username':self.config.MD_API_MAINTENANCE_USER, 'AuthString':self.config.MD_API_MAINTENANCE_PASSWORD}
        

    def connect_mdctl( self, host ):
       """
       Connect to an SMDS control daemon on a slave node
       """
       return xmlrpclib.Server( "https://" + host + ":" + str(self.config.MD_CTL_RPC_PORT) + "/RPC2" )
       
        
    def next_server_host( self ):
       """
       Get a hostname on which to spawn a metadata server
       """
       return self.config.MD_SERVER_HOSTS[random.randint(0,len(self.config.MD_SERVER_HOSTS)-1)]
    
    def all_hosts( self ):
       """
       Get all hostnames
       """
       return self.config.MD_SERVER_HOSTS
    
    def bootstrap_node( self, ssh_pkey_path, slicename, host, clean=True ):
       """
       Start/update the syndicate control daemon on a given host
       """
       bootstrap_path = os.path.join( self.config.MD_SERVICE_PATH, "tools/bootstrap.sh" )
       repository = "https://svn.princeton.edu/cdnfs"
       install_dest = self.config.MD_SERVICE_PATH
       
       scp_proc = subprocess.Popen( ["/usr/bin/scp", "-i", ssh_pkey_path, bootstrap_path, slicename + "@" + host + ":/tmp/bootstrap.sh"] )
       rc = scp_proc.wait()
       
       if rc:
          logger.error("scp rc = " + str(rc) )
          return False
       
       bootstrap_args = ["/tmp/bootstrap.sh", "-d", self.config.MD_SERVICE_PATH, "-r", repository]
       if clean:
          bootstrap_args += ["-c"]
          
       ssh_proc = subprocess.Popen( ["/usr/bin/ssh", "-i", ssh_pkey_path, slicename + "@" + host] + bootstrap_args )
       rc = ssh_proc.wait()
       
       if rc:
          logger.error("ssh bootstrap command rc = " + str(rc) )
          return False
          
       return True
    
    def boostrap_nodes(self, slicename, ssh_pkey_path, clean=True):
       """
       Install the control daemon on all given nodes
       """
       for node in self.config.MD_SERVER_HOSTS[random.randint(0,len(self.config.MD_SERVER_HOSTS))]:
          self.boostrap_node( ssh_pkey_path, slicename, node, clean )
       
    def callable(self, method):
        """
        Return a new instance of the specified method.
        """

        # Look up method
        if method not in self.all_methods:
            raise MDMethodNotFound( method )

        # Get new instance of method
        try:
            classname = method.split(".")[-1]
            fullpath="SMDS.Methods." + method
            module = __import__(fullpath, globals(), locals(), [classname])
            return getattr(module, classname)(self)
        except ImportError, AttributeError:
            raise MDMethodNotFound( "import error %s for %s" % (AttributeError,fullpath) )

    def call(self, source, method, *args):
        """
        Call the named method from the specified source with the
        specified arguments.
        """

        function = self.callable(method)
        function.source = source
        return function(*args)

    def handle(self, source, data):
        """
        Handle an XML-RPC or SOAP request from the specified source.
        """

        # Parse request into method name and arguments
        try:
            interface = xmlrpclib
            (args, method) = xmlrpclib.loads(data)
            methodresponse = True
        except Exception, e:
            if SOAPpy is not None:
                interface = SOAPpy
                (r, header, body, attrs) = parseSOAPRPC(data, header = 1, body = 1, attrs = 1)
                method = r._name
                args = r._aslist()
                # XXX Support named arguments
            else:
                raise e

        try:
            result = self.call(source, method, *args)
        except MDException, fault:
            # Handle expected faults
            if interface == xmlrpclib:
                result = fault
                methodresponse = None
            elif interface == SOAPpy:
                result = faultParameter(NS.ENV_T + ":Server", "Method Failed", method)
                result._setDetail("Fault %d: %s" % (fault.faultCode, fault.faultString))

        # Return result
        if interface == xmlrpclib:
            if not isinstance(result, MDException):
                result = (result,)
            data = xmlrpclib.dumps(result, methodresponse = True, encoding = self.encoding, allow_none = 1)
        elif interface == SOAPpy:
            data = buildSOAP(kw = {'%sResponse' % method: {'Result': result}}, encoding = self.encoding)

        return data

"""
end of borrowed code
"""
