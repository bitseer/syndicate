#ifndef _VALIDATOR_H_
#define _VALIDATOR_H_

#include "libsyndicate.h"
#include "blacklist.h"

//#define VALIDATOR_THREAD_WORKSIZE 10000

using namespace std;

int validator_init( struct md_syndicate_conf* conf );
int validator_shutdown();

/*
typedef bool (*chk_producer)(void* cls);

// ValidateHandle state types
typedef enum {
   VALIDATE_NONE,
   VALIDATE_INIT,    // validation has not started
   VALIDATE_RUN,     // validation is in progress
   VALIDATE_DONE,    // validation is complete
} validate_state;

typedef struct md_entry md_header;


// ValidateHandle status types (for the md_entry's)

#define VALIDATE_UNKNOWN 0         // unknown status
#define VALIDATE_GOOD    1         // entry is okay to keep
#define VALIDATE_BAD     2         // entry is not okay to keep


class ValidatorThreadpool;

// ValidateHandle--a state machine for metadata entry validation.
// There is one ValidateHandle per md_entry
class ValidateHandle {
   
public:
   validate_state state;
   
   CURL** curl_hs;               // list of curl handles for each entry to visit
   struct md_entry* ent;         // ent to query
   struct md_entry* queried;     // ent that we build up from queries
   
   int* url_status;     // status flags for the validation of each url
   int num_urls;        // number of urls
   
   ValidateHandle() { this->curl_hs = NULL; this->ent = NULL; this->state = VALIDATE_NONE; this->num_urls = 0; this->url_status = NULL; this->queried = NULL; }
   
   // NOTE: ent must be dynamically-allocated!  It will be freed in the ValidateHandle destructor
   ValidateHandle( struct md_entry* ents, time_t query_timeout );
   ~ValidateHandle();
   
   // update this state machine's state
   int update_state( ValidatorThreadpool* vp );
   
   // update the primary URL, given queried information.
   // return 0 if the changes were merged; negative if not
   int update_primary( md_header* primary_info, struct md_syndicate_conf* conf );
   
   // update a replica URL, given queried information.
   // return 0 if the changes were merged; negative if not
   int update_replica( md_header* replica_info, struct md_syndicate_conf* conf );
   
   // commit an entry to the master copy (either adding/updating or removing it)
   int commit_queried( struct md_syndicate_conf* conf );
   
private:
   // commit an invalid query
   int commit_invalid_query( struct md_syndicate_conf* conf );
   
   // commit a valid query
   int commit_valid_query( struct md_syndicate_conf* conf );
};


// ValidatorThreadpool--a ThreadPool that processes ValidateHandle state machines
class ValidatorThreadpool : public Threadpool<ValidateHandle>, public CURLTransfer {
public:
   
   ValidatorThreadpool( struct md_syndicate_conf* conf, Blacklist* bl, chk_producer func, void* cls );
   ~ValidatorThreadpool();
   
   void set_producer_cls(void* cls) { this->cls = cls; }
   
   // re-set the "done" status--we're gonna get some work
   void begin();
   
   // wait until nothing left to validate
   void wait( int delay );
   
   // override kill() to set done
   int kill(int sig) {
      this->done = true;
      return Threadpool<ValidateHandle>::kill( sig );
   }
   
   // add work to this threadpool
   int add_work( ValidateHandle* work );
   int add_work( ValidateHandle* work, int thread_no );
   
   // process a validator state machine, as well as run the validation process
   int process_work( ValidateHandle* work, int thread_no );
   
   // remove work from the threadpool
   int remove_work( ValidateHandle* work, int thread_no );
   
   // get the conf
   struct md_syndicate_conf* get_conf() { return this->conf; }
   
private:
   
   struct md_syndicate_conf* conf;
   Blacklist* bl;
   
   vector<ValidateHandle*>* pending;
   
   bool done;
   
   chk_producer mc_has_more;
   void* cls;
   
   // get the next ready validate handle
   ValidateHandle* next_ready_handle( int thread_no, int* which_url, int* err );
};
*/

#endif
