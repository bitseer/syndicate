#!/usr/bin/python

# many readers, 1 writer

import urllib2
import time

WRITER_PORT = 44444
WRITER_FILE = "/index.html"

ret = {}

class SyndicateHTTPRedirectHandler( urllib2.HTTPRedirectHandler ):
   def http_error_302( self, req, fp, code, msg, headers ):
      global ret
      ret['redirect_end'] = time.time()
      return urllib2.HTTPRedirectHandler(self, req, fp, code, msg, headers)

   http_error_301 = http_error_303 = http_error_304 = http_error_302



try:
   request = urllib2.Request( "http://localhost:" + str(WRITER_PORT) + WRITER_FILE )
   opener = urllib2.build_opener()

   ret['start_open'] = time.time()

   f = opener.open(request)

   ret['end_open'] = time.time()

   ret['start_recv'] = time.time()

   size = 0
   while True:
      buf = f.read(32768)
      if len(buf) == 0:
         break

      size += len(buf)

   ret['end_recv'] = time.time()
   ret['recv_len'] = size

   print ret

except Exception, e:
   ret['exception'] = str(e)
   print ret
