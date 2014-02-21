// Copyright 2010-2014 RethinkDB, all rights reserved.
#ifndef BUFFER_CACHE_ALT_PAGE_CACHE_HPP_
#define BUFFER_CACHE_ALT_PAGE_CACHE_HPP_

#include <functional>
#include <map>
#include <utility>
#include <vector>
#include <set>

#include "buffer_cache/alt/block_version.hpp"
#include "buffer_cache/alt/cache_account.hpp"
#include "buffer_cache/alt/config.hpp"
#include "buffer_cache/alt/evicter.hpp"
#include "buffer_cache/alt/free_list.hpp"
#include "buffer_cache/alt/page.hpp"
#include "buffer_cache/types.hpp"
#include "concurrency/access.hpp"
#include "concurrency/auto_drainer.hpp"
#include "concurrency/cond_var.hpp"
#include "concurrency/fifo_enforcer.hpp"
#include "concurrency/new_semaphore.hpp"
#include "containers/backindex_bag.hpp"
#include "containers/intrusive_list.hpp"
#include "containers/segmented_vector.hpp"
#include "repli_timestamp.hpp"
#include "serializer/types.hpp"

class alt_memory_tracker_t;
class auto_drainer_t;
class cache_t;
class file_account_t;

namespace alt {
class current_page_acq_t;
class page_cache_t;
class page_txn_t;

enum class page_create_t { no, yes };

}  // namespace alt

enum class alt_create_t { create };

class cache_conn_t {
public:
    explicit cache_conn_t(cache_t *cache)
        : cache_(cache),
          newest_txn_(NULL) { }
    ~cache_conn_t();

    cache_t *cache() const { return cache_; }
private:
    friend class alt::page_cache_t;
    friend class alt::page_txn_t;
    // Here for convenience, because otherwise you'd be passing around a cache_t with
    // every cache_conn_t parameter.
    cache_t *cache_;

    // The most recent unflushed txn, or NULL.  This gets set back to NULL when
    // newest_txn_ pulses its flush_complete_cond_.  It's a bidirectional pointer
    // pair with the newest txn's cache_conn_ pointer -- either both point at each
    // other or neither do.
    alt::page_txn_t *newest_txn_;

    DISABLE_COPYING(cache_conn_t);
};


namespace alt {


// Has information necessary for the current_page_t to do certain things -- it's
// known by the current_page_acq_t.
struct current_page_help_t;

class current_page_t {
public:
    // Constructs a fresh, empty page.
    current_page_t(block_size_t block_size, scoped_malloc_t<ser_buffer_t> buf,
                   page_cache_t *page_cache);
    current_page_t(scoped_malloc_t<ser_buffer_t> buf,
                   const counted_t<standard_block_token_t> &token,
                   page_cache_t *page_cache);
    // Constructs a page to be loaded from the serializer.
    current_page_t();
    ~current_page_t();


private:
    // current_page_acq_t should not access our fields directly.
    friend class current_page_acq_t;
    void add_acquirer(current_page_acq_t *acq, cache_account_t *account);
    void remove_acquirer(current_page_acq_t *acq);
    void pulse_pulsables(current_page_acq_t *acq, cache_account_t *account);

    page_t *the_page_for_write(current_page_help_t help, cache_account_t *account);
    page_t *the_page_for_read(current_page_help_t help, cache_account_t *account);

    void convert_from_serializer_if_necessary(current_page_help_t help,
                                              cache_account_t *account);

    void mark_deleted(current_page_help_t help);

    // page_txn_t should not access our fields directly.
    friend class page_txn_t;
    // Returns the previous last modifier (or NULL, if there's no active
    // last-modifying previous txn).
    page_txn_t *change_last_modifier(page_txn_t *new_last_modifier);

    // Returns NULL if the page was deleted.
    page_t *the_page_for_read_or_deleted(current_page_help_t help,
                                         cache_account_t *account);

    // Has access to our fields.
    friend class page_cache_t;

    bool is_deleted() const { return is_deleted_; }

    void make_non_deleted(block_size_t block_size,
                          scoped_malloc_t<ser_buffer_t> buf,
                          page_cache_t *page_cache);

    // page_ can be null if we haven't tried loading the page yet.  We don't want to
    // prematurely bother loading the page if it's going to be deleted.
    // the_page_for_write, the_page_for_read, or the_page_for_read_or_deleted should
    // be used to access this variable.
    // KSI: Could we encapsulate that rule?
    page_ptr_t page_;
    // True if the block is in a deleted state.  page_ will be null.
    bool is_deleted_;

    // The last txn that modified the page, or marked it deleted.
    page_txn_t *last_modifier_;

    // An in-cache value that increments whenever the value is changed, so that
    // different page_txn_t's can know which was the last to modify the block.
    block_version_t block_version_;

    // Instead of storing the recency here, we store it page_cache_t::recencies_.

    // All list elements have current_page_ != NULL, snapshotted_page_ == NULL.
    intrusive_list_t<current_page_acq_t> acquirers_;

    DISABLE_COPYING(current_page_t);
};

class current_page_acq_t : public intrusive_list_node_t<current_page_acq_t>,
                           public home_thread_mixin_debug_only_t {
public:
    // KSI: Right now we support a default constructor but buf_lock_t actually
    // uses a scoped pointer now, because getting this type to be swappable was too
    // hard.  Make this type be swappable or remove the default constructor.  (Remove
    // the page_cache_ != NULL check in the destructor we remove the default
    // constructor.)
    current_page_acq_t();
    current_page_acq_t(page_txn_t *txn,
                       block_id_t block_id,
                       access_t access,
                       cache_account_t *account,
                       page_create_t create = page_create_t::no);
    current_page_acq_t(page_txn_t *txn,
                       alt_create_t create);
    current_page_acq_t(page_cache_t *cache,
                       block_id_t block_id,
                       cache_account_t *account,
                       read_access_t read);
    ~current_page_acq_t();

    // Declares ourself snapshotted.  (You must be readonly to do this.)
    void declare_snapshotted();

    signal_t *read_acq_signal();
    signal_t *write_acq_signal();

    page_t *current_page_for_read(cache_account_t *account);
    page_t *current_page_for_write(cache_account_t *account);

    // Returns current_page_for_read, except it guarantees that the page acq has
    // already snapshotted the page and is not waiting for the page_t *.
    page_t *snapshotted_page_ptr();

    block_id_t block_id() const { return block_id_; }
    access_t access() const { return access_; }
    repli_timestamp_t recency() const;

    void mark_deleted();

    block_version_t block_version() const;

    page_cache_t *page_cache() const;

private:
    void init(page_txn_t *txn,
              block_id_t block_id,
              access_t access,
              cache_account_t *account,
              page_create_t create);
    void init(page_txn_t *txn,
              alt_create_t create);
    void init(page_cache_t *page_cache,
              block_id_t block_id,
              cache_account_t *account,
              read_access_t read);
    friend class page_txn_t;
    friend class current_page_t;

    // Returns true if the page has been created, edited, or deleted.
    bool dirtied_page() const;
    // Declares ourself readonly.  Only page_txn_t::remove_acquirer can do this!
    void declare_readonly();

    current_page_help_t help() const;

    void pulse_read_available();
    void pulse_write_available();

    page_cache_t *page_cache_;
    page_txn_t *the_txn_;
    access_t access_;
    bool declared_snapshotted_;
    // The block id of the page we acquired.
    block_id_t block_id_;
    // At most one of current_page_ is null or snapshotted_page_ is null, unless the
    // acquired page has been deleted, in which case both are null.
    current_page_t *current_page_;
    page_ptr_t snapshotted_page_;
    cond_t read_cond_;
    cond_t write_cond_;

    // The recency for our acquisition of the page.
    repli_timestamp_t recency_;

    // The block version for our acquisition of the page -- every write acquirer sees
    // a greater block version than the previous acquirer.  The current page's block
    // version will be less than or equal to this value if we have not yet acquired
    // the page.  It could be greater than this value if we're snapshotted (since
    // we're holding an old version of the page).  These values are for internal
    // cache bookkeeping only.
    block_version_t block_version_;

    bool dirtied_page_;

    DISABLE_COPYING(current_page_acq_t);
};

// This object lives on the serializer thread.
class page_read_ahead_cb_t : public home_thread_mixin_t,
                             public serializer_read_ahead_callback_t {
public:
    page_read_ahead_cb_t(serializer_t *serializer,
                         page_cache_t *cache,
                         uint64_t bytes_to_send);

    void offer_read_ahead_buf(block_id_t block_id,
                              scoped_malloc_t<ser_buffer_t> *buf,
                              const counted_t<standard_block_token_t> &token);

    void destroy_self();

private:
    ~page_read_ahead_cb_t();

    serializer_t *serializer_;
    page_cache_t *page_cache_;

    // How many more bytes of data can we send?
    uint64_t bytes_remaining_;

    DISABLE_COPYING(page_read_ahead_cb_t);
};

class tracker_acq_t {
public:
    tracker_acq_t() { }
    ~tracker_acq_t() { }
    tracker_acq_t(tracker_acq_t &&movee)
        : semaphore_acq_(std::move(movee.semaphore_acq_)) {
        movee.semaphore_acq_.reset();
    }

    // See below:  this can update how much semaphore_acq_ holds.
    void update_dirty_page_count(int64_t new_count);

private:
    friend class ::alt_memory_tracker_t;
    // At first, the number of dirty pages is 0 and semaphore_acq_.count() >=
    // dirtied_count_.  Once the number of dirty pages gets bigger than the original
    // value of semaphore_acq_.count(), we use semaphore_acq_.change_count() to keep
    // the numbers equal.
    new_semaphore_acq_t semaphore_acq_;

    DISABLE_COPYING(tracker_acq_t);
};

class page_cache_t : public home_thread_mixin_t {
public:
    page_cache_t(serializer_t *serializer,
                 const page_cache_config_t &config,
                 memory_tracker_t *tracker);
    ~page_cache_t();

    // Takes a txn to be flushed.  Calls on_flush_complete() (which resets the
    // tracker_acq parameter) when done.
    void flush_and_destroy_txn(
            scoped_ptr_t<page_txn_t> txn,
            std::function<void(tracker_acq_t *)> on_flush_complete);

    current_page_t *page_for_block_id(block_id_t block_id);
    current_page_t *page_for_new_block_id(block_id_t *block_id_out);
    current_page_t *page_for_new_chosen_block_id(block_id_t block_id);

    // Returns how much memory is being used by all the pages in the cache at this
    // moment in time.
    size_t total_page_memory() const;
    size_t evictable_page_memory() const;

    block_size_t max_block_size() const;

    cache_account_t create_cache_account(int priority);

    cache_account_t *default_reads_account() {
        return &default_reads_account_;
    }

private:
    friend class page_read_ahead_cb_t;
    void add_read_ahead_buf(block_id_t block_id,
                            ser_buffer_t *buf,
                            const counted_t<standard_block_token_t> &token);

    void have_read_ahead_cb_destroyed();

    void read_ahead_cb_is_destroyed();


    current_page_t *internal_page_for_new_chosen(block_id_t block_id);

    friend class page_t;
    evicter_t &evicter() { return evicter_; }

    // KSI: Maybe just have txn_t hold a single list of block_change_t objects.
    struct block_change_t {
        block_change_t(block_version_t _version, bool _modified,
                       page_t *_page, repli_timestamp_t _tstamp)
            : version(_version), modified(_modified), page(_page), tstamp(_tstamp) { }
        block_version_t version;

        // True if the value of the block was modified (or the block was deleted), false
        // if the block was only touched.
        bool modified;
        // If modified == true, the new value for the block, or NULL if the block was
        // deleted.  (The page_t's lifetime is kept by some page_txn_t's
        // snapshotted_dirtied_pages_ field.)
        page_t *page;
        repli_timestamp_t tstamp;
    };

    friend class page_txn_t;
    static void do_flush_changes(page_cache_t *page_cache,
                                 const std::map<block_id_t, block_change_t> &changes,
                                 fifo_enforcer_write_token_t index_write_token);
    static void do_flush_txn_set(page_cache_t *page_cache,
                                 std::map<block_id_t, block_change_t> *changes_ptr,
                                 const std::set<page_txn_t *> &txns);

    // Returns the set of page_txn_t's that have been unblocked.  The caller must
    // call im_waiting_for_flush on them (or somehow replicate its behavior).
    static MUST_USE std::set<page_txn_t *>
    remove_txn_set_from_graph(page_cache_t *page_cache,
                              const std::set<page_txn_t *> &txns);

    static std::map<block_id_t, block_change_t>
    compute_changes(const std::set<page_txn_t *> &txns);

    bool exists_flushable_txn_set(page_txn_t *txn,
                                  std::set<page_txn_t *> *flush_set_out);

    void im_waiting_for_flush(std::set<page_txn_t *> txns);

    repli_timestamp_t recency_for_block_id(block_id_t id) {
        return recencies_.size() <= id
            ? repli_timestamp_t::invalid
            : recencies_[id];
    }

    void set_recency_for_block_id(block_id_t id, repli_timestamp_t recency) {
        while (recencies_.size() <= id) {
            recencies_.push_back(repli_timestamp_t::invalid);
        }
        recencies_[id] = recency;
    }

    friend class current_page_t;
    serializer_t *serializer() { return serializer_; }
    free_list_t *free_list() { return &free_list_; }

    void resize_current_pages_to_id(block_id_t block_id);

    const page_cache_config_t dynamic_config_;

    // We use separate I/O accounts for reads and writes, so reads can pass ahead of
    // flushes.  The rationale behind this is that reads are almost always blocking
    // operations.  Writes, on the other hand, can be non-blocking (from the user's
    // perspective) if they are soft-durability or noreply writes.
    //
    // TODO: Check whether it would be better to change this to separate I/O accounts
    // for blocking and non-blocking access rather than reads and writes.  On the
    // other hand, write transactions often (always, actually, thanks metainfo block)
    // have to wait for previous ones to flush before they can proceed, so this
    // separation might be tricky in practice.
    cache_account_t default_reads_account_;
    scoped_ptr_t<file_account_t> writes_io_account_;

    // This fifo enforcement pair ensures ordering of index_write operations after we
    // move to the serializer thread and get a bunch of blocks written.
    // index_write_sink's pointee's home thread is on the serializer.
    fifo_enforcer_source_t index_write_source_;
    scoped_ptr_t<fifo_enforcer_sink_t> index_write_sink_;

    serializer_t *serializer_;
    segmented_vector_t<repli_timestamp_t> recencies_;

    // RSP: Array growth slow.
    std::vector<current_page_t *> current_pages_;

    free_list_t free_list_;

    evicter_t evicter_;

    // KSI: I bet this read_ahead_cb_ and read_ahead_cb_existence_ type could be
    // packaged in some new cross_thread_ptr type.
    page_read_ahead_cb_t *read_ahead_cb_;

    // Holds a lock on *drainer_ is until shortly after the page_read_ahead_cb_t is
    // destroyed and all possible read-ahead operations have completed.
    auto_drainer_t::lock_t read_ahead_cb_existence_;

    scoped_ptr_t<auto_drainer_t> drainer_;

    DISABLE_COPYING(page_cache_t);
};

class dirtied_page_t {
public:
    dirtied_page_t()
        : block_id(NULL_BLOCK_ID),
          tstamp(repli_timestamp_t::invalid) { }
    dirtied_page_t(block_version_t _block_version,
                   block_id_t _block_id, page_ptr_t &&_ptr,
                   repli_timestamp_t _tstamp)
        : block_version(_block_version),
          block_id(_block_id),
          ptr(std::move(_ptr)),
          tstamp(_tstamp) { }
    dirtied_page_t(dirtied_page_t &&movee)
        : block_version(movee.block_version),
          block_id(movee.block_id),
          ptr(std::move(movee.ptr)),
          tstamp(movee.tstamp) { }
    dirtied_page_t &operator=(dirtied_page_t &&movee) {
        block_version = movee.block_version;
        block_id = movee.block_id;
        ptr = std::move(movee.ptr);
        tstamp = movee.tstamp;
        return *this;
    }
    // Our block version of the dirty page.
    block_version_t block_version;
    // The block id of the dirty page.
    block_id_t block_id;
    // The pointer to the snapshotted dirty page value.  (If empty, the page was
    // deleted.)
    page_ptr_t ptr;
    // The timestamp of the modification.
    repli_timestamp_t tstamp;
};

class touched_page_t {
public:
    touched_page_t()
        : block_id(NULL_BLOCK_ID),
          tstamp(repli_timestamp_t::invalid) { }
    touched_page_t(block_version_t _block_version,
                   block_id_t _block_id,
                   repli_timestamp_t _tstamp)
        : block_version(_block_version),
          block_id(_block_id),
          tstamp(_tstamp) { }

    block_version_t block_version;
    block_id_t block_id;
    repli_timestamp_t tstamp;
};

// page_txn_t's exist for the purpose of writing to disk.  The rules are as follows:
//
//  - When a page_txn_t gets "committed" (written to disk), all blocks modified with
//    a given page_txn_t must be committed to disk at the same time.  (That is, they
//    all go in the same index_write operation.)
//
//  - For all blocks N and page_txn_t S and T, if S modifies N before T modifies N,
//    then S must be committed to disk before or at the same time as T.
//
//  - For all page_txn_t S and T, if S is the preceding_txn of T then S must be
//    committed to disk before or at the same time as T.
//
// As a result, we form a graph of txns, which gets modified on the fly, and we
// commit them in topological order.  Cycles can happen (for example, if (a) two
// transactions modify the same physical memory, or (b) they modify blocks in
// opposite orders), and transactions that depend on one another (forming a cycle)
// must get flushed simultaneously (in the same serializer->index_write operation).
// Situation '(a)' can happen as a matter of course, assuming transactions don't
// greedily save their modified copy of a page.  Situation '(b)' can happen if
// transactions apply a commutative operation on a block, like with the stats block.
// Right now, situation '(a)' doesn't happen because transactions do greedily keep
// their copies of the block.
//
// LSI: Make situation '(a)' happenable.
class page_txn_t {
public:
    // Our transaction has to get committed to disk _after_ or at the same time as
    // preceding transactions on cache_conn, if that parameter is not NULL.  (The
    // parameter's NULL for read txns, for now.)
    page_txn_t(page_cache_t *page_cache,
               // Unused for read transactions, pass repli_timestamp_t::invalid.
               repli_timestamp_t txn_recency,
               tracker_acq_t tracker_acq,
               cache_conn_t *cache_conn);

    // KSI: This is only to be called by the page cache -- should txn_t really use a
    // scoped_ptr_t?
    ~page_txn_t();

    page_cache_t *page_cache() const { return page_cache_; }

private:
    // To set cache_conn_ to NULL.
    friend class ::cache_conn_t;

    // To access tracker_acq_.
    friend class flush_and_destroy_txn_waiter_t;

    // page cache has access to all of this type's innards, including fields.
    friend class page_cache_t;

    // For access to this_txn_recency_.
    friend class current_page_t;

    // Adds and connects a preceder.
    void connect_preceder(page_txn_t *preceder);

    // Removes a preceder, which is already half-way disconnected.
    void remove_preceder(page_txn_t *preceder);

    // Removes a subseqer, which is already half-way disconnected.
    void remove_subseqer(page_txn_t *subseqer);

    // current_page_acq should only call add_acquirer and remove_acquirer.
    friend class current_page_acq_t;
    void add_acquirer(current_page_acq_t *acq);
    void remove_acquirer(current_page_acq_t *acq);

    void announce_waiting_for_flush();

    page_cache_t *page_cache_;
    // This can be NULL, if the txn is not part of some cache conn.
    cache_conn_t *cache_conn_;

    // An acquisition object for the memory tracker.
    tracker_acq_t tracker_acq_;

    repli_timestamp_t this_txn_recency_;

    // page_txn_t's form a directed graph.  preceders_ and subseqers_ represent the
    // inward-pointing and outward-pointing arrows.  (I'll let you decide which
    // direction should be inward and which should be outward.)  Each page_txn_t
    // pointed at by subseqers_ has an entry in its preceders_ that is back-pointing
    // at this page_txn_t.  (And vice versa for each page_txn_t pointed at by
    // preceders_.)

    // The transactions that must be committed before or at the same time as this
    // transaction.
    std::vector<page_txn_t *> preceders_;

    // txn's that we precede.
    // RSP: Performance?
    std::vector<page_txn_t *> subseqers_;

    // Pages for which this page_txn_t is the last_modifier_ of that page.
    std::vector<current_page_t *> pages_modified_last_;

    // acqs that are currently alive.
    // RSP: Performance?  remove_acquirer takes linear time.
    std::vector<current_page_acq_t *> live_acqs_;

    // Saved pages (by block id).
    segmented_vector_t<dirtied_page_t, 8> snapshotted_dirtied_pages_;

    // Touched pages (by block id).
    segmented_vector_t<touched_page_t, 8> touched_pages_;

    // KSI: We could probably turn began_waiting_for_flush_ and spawned_flush_ into a
    // generalized state enum.
    //
    // KSI: Should we have the spawned_flush_ variable or should we remove the txn
    // from the graph?

    // Tells whether this page_txn_t has announced itself (to the cache) to be
    // waiting for a flush.
    bool began_waiting_for_flush_;
    bool spawned_flush_;

    // This gets pulsed when the flush is complete or when the txn has no reason to
    // exist any more.
    cond_t flush_complete_cond_;

    DISABLE_COPYING(page_txn_t);
};

}  // namespace alt


#endif  // BUFFER_CACHE_ALT_PAGE_CACHE_HPP_
