//! Data structures for maintaining lists of hits in ways that make merging results from parallel threads easy
/*! How these structures work:  
 * A P7_HIT_CHUNK contains and describes a set of hits, which must be sorted in order of ascending object  ID.  
 * A P7_HITLIST contains either the entire set of hits found by a node or the entire set of hits found during a search. 
 *
 * P7_HIT_CHUNKs are typically generated by worker threads.  Worker threads search regions of a database in ascending order
 * by object ID.  When they find a hit, they add it to their P7_HIT_CHUNK.  When a worker thread finishes a region and needs to start
 * another, it inserts its chunk into the node's P7_HITLIST structure and starts a new one.  
*
* A P7_HITLIST contains a linked list of hits, sorted by object ID, and also a linked list of P7_HIT_CHUNKs, again sorted by object ID
* The P7_HIT_CHUNKs must have non-overlapping ranges of object IDs.  This will happen naturally when merging the chunks generated
* by the threads running on a single node, but merging the results from multiple machines will require merging the chunks by hand
* To insert a chunk into a hitlist, search the list of chunks in the list until you find the right place to insert the new chunk.  Splice it into 
* the list of chunks, and also splice the hits in the chunk into the full list
*/

#ifndef p7HITLIST_INCLUDED
#define p7HITLIST_INCLUDED

#include "base/p7_tophits.h"
#include "esl_red_black.h"

//#define HITLIST_SANITY_CHECK  // conditionally compiles code to check the hitlist every time it's modified.  
// don't define for releases, will be slow
struct p7_daemon_workernode_state;
struct p7_daemon_masternode_state;


#define HITLIST_POOL_SIZE 1000 // default size of each engine's hitlist pool
#define HIT_MESSAGE_LIMIT 100000 // soft upper limit on the size of each message containing hits. When sending hits, we create a new
//message whenever the current one exceeds this limit, so actual maximum size is limit + sizeof(last hit) -1

//! Entry used to form a doubly-linked list of hits
/*! Invariant: hits in the list are required to be sorted in ascending order by object id  */
typedef struct p7_hitlist_entry{
	P7_HIT *hit; 
	struct p7_hitlist_entry *prev;
	struct p7_hitlist_entry *next;
} P7_HITLIST_ENTRY;

//! Structure that holds a chunk of hits, sorted by object id
typedef struct p7_hit_chunk{
	//! Beginning entry in the list
	P7_HITLIST_ENTRY *start;

	//! Last entry in the list
	P7_HITLIST_ENTRY *end;

	//! object ID of the first entry in the list
	uint64_t start_id;

	//! object ID of the last entry in the list
	uint64_t end_id;

	//Previous chunk in the list
	struct p7_hit_chunk *prev;

	//Next chunk in the list
	struct p7_hit_chunk *next;
} P7_HIT_CHUNK;

//! Holds the full list of hits that a machine has found
typedef struct p7_hitlist{
	//! lock used to serialize changes to the hitlist
	pthread_mutex_t lock;

	//! lowest-ID hit in the list
	P7_HITLIST_ENTRY *hit_list_start;

	//! highest-ID hit in the list
	P7_HITLIST_ENTRY *hit_list_end;

	//! object ID of the first entry in the list
	uint64_t hit_list_start_id;

	//! object ID of the last entry in the list
	uint64_t hit_list_end_id;

	//! Start of the list of chunks
	P7_HIT_CHUNK *chunk_list_start;

	//! End of the list of chunks
	P7_HIT_CHUNK *chunk_list_end;

#ifdef HITLIST_SANITY_CHECK
	uint64_t num_hits; // counter for number of hits, used to check consistency of the list
#endif

} P7_HITLIST;


//Functions to create and manipulate P7_HITLIST_ENTRY objects


ESL_RED_BLACK_DOUBLEKEY *p7_get_hit_tree_entry_from_pool(struct p7_daemon_workernode_state *workernode, uint32_t my_id);


/*! NOTE: NOT THREADSAFE.  ASSUMES ONLY ONE THREAD PULLING ENTRIES FROM POOL */
ESL_RED_BLACK_DOUBLEKEY *p7_get_hit_tree_entry_from_masternode_pool(struct p7_daemon_masternode_state *masternode);
//! Creates a P7_HITLIST_ENTRY object and its included P7_HIT object
P7_HITLIST_ENTRY *p7_hitlist_entry_Create();


//! Creates a linked list of num_entries esl_red_black_doublekey nodes whose contents are hitlist entries and returns it
ESL_RED_BLACK_DOUBLEKEY *p7_hitlist_entry_pool_Create(uint32_t num_entries);

//! Destroys a P7_HITLIST_ENTRY object and its included P7_HIT object.
/*! NOTE:  do not call the base p7_hit_Destroy function on the P7_HIT object in a P7_HITLIST_ENTRY.  
 * p7_hit_Destroy calls free on some of the objects internal to the P7_HIT object.  In the hitlist, these are pointers 
 * into the daemon's data shard, so freeing them will break things very badly
 * @param the_entry the hitlist entry to be destroyed.
 */
void p7_hitlist_entry_Destroy(P7_HITLIST_ENTRY *the_entry);






//Functions to create and manipulate P7_HIT_CHUNK objects

// Functions to create and manipolate P7_HITLIST objects

//! creates and returns a new, empty hitlist
P7_HITLIST *p7_hitlist_Create();

//! Destroys a hitlist and frees its memory
/*! @param the_list the list to be destroyed */
void p7_hitlist_Destroy(P7_HITLIST *the_list, struct p7_daemon_workernode_state *workernode);

// dummy output printing function for testing
void p7_print_hitlist(char *filename, P7_HITLIST *th);
void p7_print_and_recycle_hit_tree(char *filename, ESL_RED_BLACK_DOUBLEKEY *tree, struct p7_daemon_masternode_state *masternode);


// Functions to send and receive hits using the tree-based data structures

//! Takes an unsorted list of hits (red-black tree objects, chained through the large pointer) and sends them via MPI
/*! also recycles the red-black tree objects, returning them to the workernode structure
    @param hits the list of hits to be sent.  Hits may be sent as multiple messages
    @param nhits the number of hits in the list (needed so that we can make it the first item in the message)
    @param dest the destination MPI process that the hits should be sent to (master node)
	@param tag the tag to apply to MPI messages
	@param comm the MPI communicator to use
	@param buf pointer to the buffer that will hold the message.  Typed as pointer-to-pointer to allow re-allocation of the buffer if needed
	@param nalloc size of the buffer in bytes.  May be updated if the buffer is resized.
	@param workernode the workernode structure that the red-black tree entries should be recycled into 
	@return ESLOK if the data is sent successfully, ESLFAIL if not */

int p7_mpi_send_and_recycle_unsorted_hits(ESL_RED_BLACK_DOUBLEKEY *hits, int dest, int tag, MPI_Comm comm, char **buf, int *nalloc, struct p7_daemon_workernode_state *workernode);


//! Receives an unsorted list of hits via MPI and adds them to the master node's sorted tree of hits
/*! @param comm the MPI communicater to use
	@param buf the buffer that will hold the hit objects once received.  This buffer provides the raw storage for the hits, which will
	also be inserted into red-black tree objects and added to the master node's hit tree.  This is a pointer-to-pointer so that the buffer can be resized if necessary
	@param nalloc the size of the buffer in bytes.  May be updated if the buffer is resized.
	@masternode the masternode structure that will contain the sorted hit tree. */

int p7_mpi_recv_and_sort_hits(MPI_Comm comm, char **buf, int *nalloc, struct p7_daemon_masternode_state *masternode);

#endif // p7HITLIST_INCLUDED
