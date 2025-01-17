/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2012-2020 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */
#include "config.h"
#include "iotests.h"
#include "internal.h"
#include <map>
#include <include/libcouchbase/utils.h>

#include "capi/cmd_observe.hh"
#include "capi/cmd_endure.hh"

using namespace std;

#define LOGARGS(instance, lvl) instance->settings, "tests-dur", LCB_LOG_##lvl, __FILE__, __LINE__
#define SECS_USECS(f) ((f)*1000000)

static bool supportsMutationTokens(lcb_INSTANCE *instance)
{
    // Ensure we have at least one connection
    storeKey(instance, "dummy_stok_test", "dummy");

    int val = 0;
    lcb_STATUS rc;
    rc = lcb_cntl(instance, LCB_CNTL_GET, LCB_CNTL_MUTATION_TOKENS_SUPPORTED, &val);

    EXPECT_EQ(LCB_SUCCESS, rc);
    if (val == 0) {
        printf("Current cluster does not support synctokens!\n");
        return false;
    } else {
        return true;
    }
}

class DurabilityUnitTest : public MockUnitTest
{
  protected:
    static void defaultOptions(lcb_INSTANCE *instance, lcb_durability_opts_st &opts)
    {
        lcb_size_t nservers = lcb_get_num_nodes(instance);
        lcb_size_t nreplicas = lcb_get_num_replicas(instance);

        opts.v.v0.persist_to = (lcb_uint16_t)min(nreplicas + 1, nservers);
        opts.v.v0.replicate_to = (lcb_uint16_t)min(nreplicas, nservers - 1);
    }
};

extern "C" {
static void defaultDurabilityCallback(lcb_INSTANCE *, int, const lcb_RESPENDURE *);
static void multiDurabilityCallback(lcb_INSTANCE *, int, const lcb_RESPENDURE *);
}

class DurabilityOperation
{
  public:
    DurabilityOperation() = default;

    string key;
    lcb_RESPENDURE resp_{};
    lcb_CMDENDURE req{};

    void assign(const lcb_RESPENDURE *resp)
    {
        resp_ = *resp;
        key = resp->ctx.key;
    }

    static void wait(lcb_INSTANCE *instance)
    {
        lcb_install_callback(instance, LCB_CALLBACK_ENDURE, (lcb_RESPCALLBACK)defaultDurabilityCallback);
        EXPECT_EQ(LCB_SUCCESS, lcb_wait(instance, LCB_WAIT_DEFAULT));
    }

    void wait(lcb_INSTANCE *instance, const lcb_durability_opts_t *opts, const lcb_CMDENDURE *cmd,
              lcb_STATUS expected = LCB_SUCCESS)
    {

        lcb_STATUS rc;
        lcb_MULTICMD_CTX *mctx = lcb_endure3_ctxnew(instance, opts, &rc);
        EXPECT_FALSE(mctx == nullptr);
        rc = mctx->add_endure(mctx, cmd);
        EXPECT_EQ(expected, rc);
        if (rc != LCB_SUCCESS) {
            mctx->fail(mctx);
        } else {
            rc = mctx->done(mctx, this);
            EXPECT_EQ(LCB_SUCCESS, rc);
            wait(instance);
        }
    }

    void run(lcb_INSTANCE *instance, const lcb_durability_opts_t *opts, const Item &itm,
             lcb_STATUS expected = LCB_SUCCESS)
    {
        lcb_CMDENDURE cmd = {0};
        ASSERT_FALSE(itm.key.empty());
        LCB_CMD_SET_KEY(&cmd, itm.key.data(), itm.key.length());
        cmd.cas = itm.cas;
        wait(instance, opts, &cmd, expected);
    }

    // Really wait(), but named as 'run()' here to make usage more consistent.
    void run(lcb_INSTANCE *instance, const lcb_durability_opts_t *opts, const lcb_CMDENDURE &cmd)
    {
        wait(instance, opts, &cmd);
    }

    void assertCriteriaMatch(const lcb_durability_opts_st &opts) const
    {
        ASSERT_EQ(LCB_SUCCESS, resp_.ctx.rc);
        ASSERT_TRUE(resp_.persisted_master != 0);
        ASSERT_TRUE(opts.v.v0.persist_to <= resp_.npersisted);
        ASSERT_TRUE(opts.v.v0.replicate_to <= resp_.nreplicated);
    }

    void dump(std::string &str) const
    {
        if (key.empty()) {
            str = "<No Key>\n";
            return;
        }
        std::stringstream ss;
        ss << "Key: " << key << std::endl
           << "Error: " << resp_.ctx.rc << std::endl
           << "Persisted (master?): " << resp_.npersisted << " (" << resp_.persisted_master << ")" << std::endl
           << "Replicated: " << resp_.nreplicated << std::endl
           << "CAS: 0x" << std::hex << resp_.ctx.cas << std::endl;
        str += ss.str();
    }

    void dump() const
    {
        string s;
        dump(s);
        cout << s;
    }
};

class DurabilityMultiOperation
{
  public:
    DurabilityMultiOperation() : counter(0) {}

    template <typename T>
    void run(lcb_INSTANCE *instance, const lcb_durability_opts_t *opts, const T &items)
    {
        counter = 0;
        unsigned ii = 0;
        lcb_STATUS rc;
        lcb_MULTICMD_CTX *mctx = lcb_endure3_ctxnew(instance, opts, &rc);
        ASSERT_FALSE(mctx == nullptr);

        for (const auto &itm : items) {
            lcb_CMDENDURE cmd = {0};

            cmd.cas = itm.cas;
            LCB_CMD_SET_KEY(&cmd, itm.key.c_str(), itm.key.length());
            rc = mctx->add_endure(mctx, &cmd);
            ASSERT_EQ(LCB_SUCCESS, rc);
            kmap[itm.key] = DurabilityOperation();
        }

        lcb_install_callback(instance, LCB_CALLBACK_ENDURE, (lcb_RESPCALLBACK)multiDurabilityCallback);

        rc = mctx->done(mctx, this);
        ASSERT_EQ(LCB_SUCCESS, rc);
        lcb_wait(instance, LCB_WAIT_DEFAULT);
        ASSERT_EQ(items.size(), counter);
    }

    void assign(const lcb_RESPENDURE *resp)
    {
        ASSERT_FALSE(resp->ctx.key.empty());
        counter++;

        string key = resp->ctx.key;
        ASSERT_TRUE(kmap.find(key) != kmap.end());
        kmap[key].assign(resp);
    }

    template <typename T>
    bool _findItem(const string &s, const T &items, Item &itm)
    {
        for (const auto &item : items) {
            if (item.key == s) {
                itm = item;
                return true;
            }
        }
        return false;
    }

    template <typename T>
    void assertAllMatch(const lcb_durability_opts_t &opts, const T &items_ok, const T &items_missing,
                        lcb_STATUS missing_err = LCB_ERR_DOCUMENT_NOT_FOUND)
    {

        for (const auto &operation : kmap) {
            Item itm_tmp;
            // make sure we were expecting it
            if (_findItem(operation.second.key, items_ok, itm_tmp)) {
                operation.second.assertCriteriaMatch(opts);

            } else if (_findItem(operation.second.key, items_missing, itm_tmp)) {
                ASSERT_EQ(missing_err, operation.second.resp_.ctx.rc);

            } else {
                ASSERT_STREQ("", "Key not in missing or OK list");
            }
        }

        // Finally, make sure they're all there

        for (const auto &item : items_ok) {
            ASSERT_TRUE(kmap.find(item.key) != kmap.end());
        }

        for (const auto &item : items_missing) {
            ASSERT_TRUE(kmap.find(item.key) != kmap.end());
        }
    }

    unsigned counter;
    map<string, DurabilityOperation> kmap;
};

extern "C" {
static void defaultDurabilityCallback(lcb_INSTANCE *, int, const lcb_RESPENDURE *res)
{
    ((DurabilityOperation *)res->cookie)->assign(res);
}
static void multiDurabilityCallback(lcb_INSTANCE *, int, const lcb_RESPENDURE *res)
{
    ((DurabilityMultiOperation *)res->cookie)->assign(res);
}
}

TEST_F(DurabilityUnitTest, testInvalidCriteria)
{
    /**
     * We don't schedule stuff to the network here
     */
    HandleWrap hwrap;
    createConnection(hwrap);

    lcb_INSTANCE *instance = hwrap.getLcb();

    lcb_durability_opts_t opts = {0};
    defaultOptions(instance, opts);
    opts.v.v0.persist_to = 10;
    opts.v.v0.replicate_to = 100;
    opts.v.v0.cap_max = 0;

    lcb_MULTICMD_CTX *mctx;
    lcb_STATUS err = LCB_SUCCESS;
    mctx = lcb_endure3_ctxnew(instance, &opts, &err);
    ASSERT_EQ(err, LCB_ERR_DURABILITY_TOO_MANY);
    ASSERT_EQ((lcb_MULTICMD_CTX *)nullptr, mctx);
}

/**
 * Test various criteria for durability
 */
TEST_F(DurabilityUnitTest, testDurabilityCriteria)
{
    HandleWrap hwrap;
    lcb_INSTANCE *instance;

    createConnection(hwrap);
    instance = hwrap.getLcb();

    lcb_durability_opts_st opts = {0};

    /** test with no persist/replicate */
    defaultOptions(instance, opts);

    opts.v.v0.replicate_to = 0;
    opts.v.v0.persist_to = 0;

    lcb_MULTICMD_CTX *mctx;
    lcb_STATUS err = LCB_SUCCESS;
    mctx = lcb_endure3_ctxnew(instance, &opts, &err);
    ASSERT_EQ(err, LCB_ERR_INVALID_ARGUMENT);
    ASSERT_EQ((lcb_MULTICMD_CTX *)nullptr, mctx);
}

/**
 * @test Test several 'basic' durability functions
 *
 * @pre Store a key. Perform a durability check with master-only persistence
 * (i.e. persist_to = 1, replicate_to = 0)
 * @post Operation succeeds
 *
 * @pre Check the key against 'maximum possible' durability by estimating the
 * maximum replica/server count
 *
 * @post Operation succeeds
 *
 * @pre Set the durability options to a very large criteria, but set the
 * @c cap_max flag so the API will reduce it to a sane default. Then use it
 * for a durability check
 *
 * @post the response is successful
 */
TEST_F(DurabilityUnitTest, testSimpleDurability)
{
    /** need real cluster for durability tests */
    LCB_TEST_REQUIRE_FEATURE("observe")
    SKIP_UNLESS_MOCK()

    HandleWrap hwrap;
    lcb_INSTANCE *instance;

    Item kv = Item("a_key", "a_value", 0);
    createConnection(hwrap);
    instance = hwrap.getLcb();

    removeKey(instance, kv.key);

    KVOperation kvo = KVOperation(&kv);
    kvo.store(instance);

    // Now wait for it to persist
    lcb_durability_opts_t opts;
    memset(&opts, 0, sizeof(opts));
    opts.v.v0.pollopts = LCB_DURABILITY_MODE_SEQNO;
    opts.v.v0.persist_to = 1;
    opts.v.v0.replicate_to = 0;

    kvo = KVOperation(&kv);
    kvo.get(instance);

    DurabilityOperation dop;
    dop.run(instance, &opts, kvo.result);

    dop.assertCriteriaMatch(opts);
    ASSERT_STREQ(kv.key.c_str(), dop.key.c_str());

    // Try with more expanded criteria
    defaultOptions(instance, opts);
    dop = DurabilityOperation();
    dop.run(instance, &opts, kvo.result);
    dop.assertCriteriaMatch(opts);

    // Make the options to some absurd number. Ensure it's capped!
    opts.v.v0.persist_to = 100;
    opts.v.v0.replicate_to = 100;
    opts.v.v0.cap_max = 1;

    dop = DurabilityOperation();
    dop.run(instance, &opts, kvo.result);
    defaultOptions(instance, opts);
    dop.assertCriteriaMatch(opts);
}

/**
 * @test Durability checks against non-existent keys
 * @pre Remove a key, and perform a durability check against it
 * @post Operation fails with @c LCB_ERR_DOCUMENT_NOT_FOUND
 */
TEST_F(DurabilityUnitTest, testNonExist)
{
    LCB_TEST_REQUIRE_FEATURE("observe")
    SKIP_UNLESS_MOCK()

    lcb_INSTANCE *instance;
    HandleWrap hwrap;

    string key = "non-exist-key";

    createConnection(hwrap);
    instance = hwrap.getLcb();

    removeKey(instance, key);

    Item itm = Item(key, "", 0);

    DurabilityOperation dop;
    lcb_durability_opts_t opts = {0};
    opts.v.v0.timeout = SECS_USECS(2);

    defaultOptions(instance, opts);

    opts.version = 1;
    opts.v.v0.pollopts = LCB_DURABILITY_MODE_SEQNO;

    dop.run(instance, &opts, itm, LCB_ERR_DURABILITY_NO_MUTATION_TOKENS);
}

/**
 * @test Test negative durability (Delete)
 *
 * @pre Store a key, Remove it, perform a durability check against the key,
 * using the @c check_delete flag
 *
 * @post A positive reply is received indicating the item has been deleted
 *
 * @pre Store the key, but don't remove it. Perform a durability check against
 * the key using the delete flag
 *
 * @post Operation is returned with @c LCB_ERR_TIMEOUT
 */
TEST_F(DurabilityUnitTest, testDelete)
{
    LCB_TEST_REQUIRE_FEATURE("observe")
    SKIP_UNLESS_MOCK()

    HandleWrap hwrap;
    lcb_INSTANCE *instance;
    lcb_durability_opts_t opts = {0};
    string key = "deleted-key";
    createConnection(hwrap);
    instance = hwrap.getLcb();

    storeKey(instance, key, "value");

    Item itm = Item(key, "value", 0);
    KVOperation kvo = KVOperation(&itm);
    kvo.remove(instance);

    // Ensure the key is actually purged!
    MockMutationCommand mcmd(MockCommand::PURGE, key);
    mcmd.onMaster = true;
    mcmd.replicaCount = lcb_get_num_replicas(instance);
    doMockTxn(mcmd);

    defaultOptions(instance, opts);
    opts.v.v0.check_delete = 1;
    DurabilityOperation dop;
    dop.run(instance, &opts, itm);
    dop.assertCriteriaMatch(opts);

    kvo.clear();
    kvo.request = &itm;
    kvo.store(instance);

    opts.v.v0.timeout = SECS_USECS(1);

    opts.version = 1;
    opts.v.v0.pollopts = LCB_DURABILITY_MODE_SEQNO;
    dop = DurabilityOperation();
    dop.run(instance, &opts, itm);
    ASSERT_EQ(LCB_SUCCESS, dop.resp_.ctx.rc);
}

/**
 * @test Test behavior when a key is modified (exists with a different CAS)
 *
 * @pre Store a key. Store it again. Keep the CAS from the first store as the
 * stale CAS. Keep the current CAS as well.
 *
 * @pre Perform a durability check against the stale CAS
 * @post Operation fails with @c LCB_ERR_DOCUMENT_EXISTS
 *
 * @pre Perform a durability check against the new CAS
 * @post Operation succeeds
 */
TEST_F(DurabilityUnitTest, testModified)
{
    LCB_TEST_REQUIRE_FEATURE("observe")

    HandleWrap hwrap;
    lcb_INSTANCE *instance;
    lcb_durability_opts_t opts = {0};
    string key = "mutated-key";
    Item itm = Item(key, key);
    KVOperation kvo_cur(&itm), kvo_stale(&itm);

    createConnection(hwrap);
    instance = hwrap.getLcb();

    kvo_stale.store(instance);
    kvo_cur.store(instance);

    kvo_stale.result.val = kvo_cur.result.val = key;

    defaultOptions(instance, opts);

    opts.version = 1;
    opts.v.v0.pollopts = LCB_DURABILITY_MODE_SEQNO;
    DurabilityOperation dop;
    dop.run(instance, &opts, kvo_stale.result);
    ASSERT_EQ(LCB_SUCCESS, dop.resp_.ctx.rc);
}

/**
 * @test Test with very quick timeouts
 * @pre Schedule an operation with an interval of 2 usec and a timeout of
 * 5 usec
 *
 * @post Operation returns with LCB_ERR_TIMEOUT
 */
TEST_F(DurabilityUnitTest, testQuickTimeout)
{
    LCB_TEST_REQUIRE_FEATURE("observe")
    lcb_INSTANCE *instance;
    HandleWrap hwrap;
    lcb_durability_opts_t opts = {0};
    string key = "a_key";

    createConnection(hwrap);
    instance = hwrap.getLcb();

    Item itm = Item(key, key);
    KVOperation(&itm).store(instance);

    defaultOptions(instance, opts);

    /* absurd */
    opts.v.v0.timeout = 5;
    opts.v.v0.interval = 2;

    for (unsigned ii = 0; ii < 10; ii++) {
        DurabilityOperation dop;
        dop.run(instance, &opts, itm);
        ASSERT_EQ(LCB_ERR_TIMEOUT, dop.resp_.ctx.rc);
    }
}

/**
 * @test Test a durability request for multiple keys
 *
 * @pre Store ten keys, and check that they exist all at once
 * @post all ten keys are received in the response, and they're ok
 */
TEST_F(DurabilityUnitTest, testMulti)
{
    LCB_TEST_REQUIRE_FEATURE("observe")
    unsigned ii;
    const unsigned limit = 10;

    vector<Item> items_stored;

    HandleWrap hwrap;
    lcb_INSTANCE *instance;

    createConnection(hwrap);
    instance = hwrap.getLcb();
    // Set the timeout to something high. For some reason this gives problem
    // on a real cluster
    lcb_cntl_setu32(instance, LCB_CNTL_DURABILITY_TIMEOUT, LCB_MS2US(10000));

    for (ii = 0; ii < limit; ii++) {
        char buf[64];
        sprintf(buf, "key-stored-%u", ii);
        string key_stored = buf;

        removeKey(instance, key_stored);

        Item itm_e = Item(key_stored, key_stored, 0);

        KVOperation kvo(&itm_e);
        kvo.store(instance);
        items_stored.push_back(kvo.result);
    }

    lcb_durability_opts_t opts = {0};
    defaultOptions(instance, opts);
    opts.version = 1;
    opts.v.v0.pollopts = LCB_DURABILITY_MODE_SEQNO;

    /**
     * Create the command..
     */
    DurabilityMultiOperation dmop = DurabilityMultiOperation();
    dmop.run(instance, &opts, items_stored);
    dmop.assertAllMatch(opts, items_stored, vector<Item>());
}

struct cb_cookie {
    int is_observe;
    int count;
};

extern "C" {
static void dummyObserveCallback(lcb_INSTANCE *, lcb_CALLBACK_TYPE, const lcb_RESPOBSERVE *resp)
{
    auto *c = (struct cb_cookie *)resp->cookie;
    ASSERT_EQ(1, c->is_observe);
    c->count++;
}

static void dummyDurabilityCallback(lcb_INSTANCE *, lcb_CALLBACK_TYPE, const lcb_RESPENDURE *resp)
{
    auto *c = (struct cb_cookie *)resp->cookie;
    ASSERT_EQ(0, c->is_observe);
    c->count++;
}
}

/**
 * @test Ensure basic observe functions as normal
 *
 * @pre pair up two batched commands, one a durability command, and one a
 * primitive observe. Set up distinct callbacks for the two (both of which
 * touch a counter, one incrementing and one decrementing an int*).
 * Wait for the operations to complete via @c lcb_wait
 *
 * @post The durability counter is decremented, observe counter incremented
 */
TEST_F(DurabilityUnitTest, testObserveSanity)
{
    LCB_TEST_REQUIRE_FEATURE("observe")
    HandleWrap handle;
    lcb_INSTANCE *instance;
    createConnection(handle);
    instance = handle.getLcb();

    lcb_install_callback(instance, LCB_CALLBACK_ENDURE, (lcb_RESPCALLBACK)dummyDurabilityCallback);
    lcb_install_callback(instance, LCB_CALLBACK_OBSERVE, (lcb_RESPCALLBACK)dummyObserveCallback);

    storeKey(instance, "key", "value");

    struct cb_cookie o_cookie = {1, 0};
    {
        lcb_MULTICMD_CTX *mctx = lcb_observe3_ctxnew(instance);
        ASSERT_NE((lcb_MULTICMD_CTX *)nullptr, mctx);
        lcb_CMDOBSERVE cmd = {0};
        LCB_CMD_SET_KEY(&cmd, "key", 3);
        ASSERT_EQ(LCB_SUCCESS, mctx->add_observe(mctx, &cmd));
        ASSERT_EQ(LCB_SUCCESS, mctx->done(mctx, &o_cookie));
    }

    struct cb_cookie d_cookie = {0, 0};
    {
        lcb_durability_opts_t opts = {0};
        defaultOptions(instance, opts);

        lcb_STATUS err = LCB_SUCCESS;
        lcb_MULTICMD_CTX *mctx = lcb_endure3_ctxnew(instance, &opts, &err);
        ASSERT_EQ(LCB_SUCCESS, err);
        ASSERT_NE((lcb_MULTICMD_CTX *)nullptr, mctx);
        lcb_CMDENDURE cmd = {0};
        LCB_CMD_SET_KEY(&cmd, "key", 3);
        ASSERT_EQ(LCB_SUCCESS, mctx->add_endure(mctx, &cmd));
        ASSERT_EQ(LCB_SUCCESS, mctx->done(mctx, &d_cookie));
    }

    ASSERT_EQ(LCB_SUCCESS, lcb_wait(instance, LCB_WAIT_DEFAULT));

    ASSERT_GT(o_cookie.count, 0);
    ASSERT_GT(d_cookie.count, 0);
}

TEST_F(DurabilityUnitTest, testMasterObserve)
{
    LCB_TEST_REQUIRE_FEATURE("observe")
    SKIP_UNLESS_MOCK()

    HandleWrap handle;
    createConnection(handle);
    lcb_INSTANCE *instance = handle.getLcb();

    lcb_install_callback(instance, LCB_CALLBACK_OBSERVE, (lcb_RESPCALLBACK)dummyObserveCallback);

    struct cb_cookie o_cookie = {1, 0};
    lcb_MULTICMD_CTX *mctx = lcb_observe3_ctxnew(instance);
    ASSERT_NE((lcb_MULTICMD_CTX *)nullptr, mctx);
    lcb_CMDOBSERVE cmd = {0};
    cmd.cmdflags |= LCB_CMDOBSERVE_F_MASTER_ONLY;
    LCB_CMD_SET_KEY(&cmd, "key", 3);
    ASSERT_EQ(LCB_SUCCESS, mctx->add_observe(mctx, &cmd));
    ASSERT_EQ(LCB_SUCCESS, mctx->done(mctx, &o_cookie));
    lcb_wait(instance, LCB_WAIT_DEFAULT);

    // 2 == one for the callback, one for the nullptr
    ASSERT_EQ(2, o_cookie.count);
}

extern "C" {
static void fo_callback(void *cookie)
{
    auto *instance = (lcb_INSTANCE *)cookie;
    MockEnvironment *mock = MockEnvironment::getInstance();
    for (int ii = 1; ii < mock->getNumNodes(); ii++) {
        mock->failoverNode(ii);
    }
    lcb_loop_unref(instance);
}
}

/**
 * Test the functionality of durability operations during things like
 * node failovers.
 *
 * The idea behind here is to ensure that we can trigger a case where a series
 * of OBSERVE packets are caught in the middle of a cluster update and end up
 * being relocated to the same server. Previously (and currently) this would
 * confuse the lookup_server_with_command functionality which would then invoke
 * the 'nullptr' callback multiple times (because it assumes it's not located
 * anywhere else)
 */
TEST_F(DurabilityUnitTest, testDurabilityRelocation)
{
    SKIP_UNLESS_MOCK()

    // Disable CCCP so that we get streaming updates
    MockEnvironment *mock = MockEnvironment::getInstance();
    mock->setCCCP(false);

    HandleWrap handle;
    lcb_INSTANCE *instance;
    createConnection(handle);
    instance = handle.getLcb();

    lcb_install_callback(instance, LCB_CALLBACK_ENDURE, (lcb_RESPCALLBACK)dummyDurabilityCallback);

    std::string key = "key";
    lcb_durability_opts_t opts = {0};
    opts.v.v0.persist_to = 100;
    opts.v.v0.replicate_to = 100;
    opts.v.v0.cap_max = 1;
    storeKey(instance, key, "value");

    // Ensure we have to resend commands multiple times
    MockMutationCommand mcmd(MockCommand::UNPERSIST, key);
    mcmd.onMaster = true;
    mcmd.replicaCount = lcb_get_num_replicas(instance);
    doMockTxn(mcmd);

    /**
     * Failover all but one node
     */
    for (int ii = 1; ii < mock->getNumNodes(); ii++) {
        mock->hiccupNodes(1000, 0);
    }
    lcbio_pTIMER tm = lcbio_timer_new(handle.getLcb()->iotable, instance, fo_callback);
    lcbio_timer_rearm(tm, 500000);
    lcb_loop_ref(instance);

    lcb_STATUS err = LCB_SUCCESS;
    lcb_MULTICMD_CTX *mctx = lcb_endure3_ctxnew(instance, &opts, &err);
    ASSERT_EQ(LCB_SUCCESS, err);
    ASSERT_NE((lcb_MULTICMD_CTX *)nullptr, mctx);
    lcb_CMDENDURE cmd = {0};
    LCB_CMD_SET_KEY(&cmd, key.c_str(), key.size());
    err = mctx->add_endure(mctx, &cmd);
    ASSERT_EQ(LCB_SUCCESS, err);

    struct cb_cookie cookie = {0, 0};
    ASSERT_EQ(LCB_SUCCESS, mctx->done(mctx, &cookie));

    lcb_wait(instance, LCB_WAIT_DEFAULT);
    lcbio_timer_destroy(tm);
    ASSERT_EQ(1, cookie.count);
}

TEST_F(DurabilityUnitTest, testMissingSynctoken)
{
    HandleWrap hw;
    lcb_INSTANCE *instance;
    createConnection(hw, &instance);

    if (!supportsMutationTokens(instance)) {
        return;
    }

    std::string("nonexist-key");
    lcb_STATUS rc;
    lcb_MULTICMD_CTX *mctx;
    lcb_durability_opts_t options = {0};
    defaultOptions(instance, options);
    options.version = 1;
    options.v.v0.pollopts = LCB_DURABILITY_MODE_SEQNO;

    mctx = lcb_endure3_ctxnew(instance, &options, &rc);
    ASSERT_FALSE(mctx == nullptr);
    lcb_CMDENDURE cmd = {0};
    LCB_CMD_SET_KEY(&cmd, "foo", 3);

    rc = mctx->add_endure(mctx, &cmd);
    ASSERT_EQ(LCB_ERR_DURABILITY_NO_MUTATION_TOKENS, rc);

    mctx->fail(mctx);
}

TEST_F(DurabilityUnitTest, testExternalSynctoken)
{
    HandleWrap hw1, hw2;
    lcb_INSTANCE *instance1, *instance2;
    createConnection(hw1, &instance1);
    createConnection(hw2, &instance2);

    if (!supportsMutationTokens(instance1)) {
        return;
    }

    std::string key("hello");
    std::string value("world");
    storeKey(instance1, key, value);

    const lcb_MUTATION_TOKEN *ss;
    lcb_KEYBUF kb;
    lcb_STATUS rc;
    LCB_KREQ_SIMPLE(&kb, key.c_str(), key.size());
    ss = lcb_get_mutation_token(instance1, &kb, &rc);
    ASSERT_FALSE(ss == nullptr);
    ASSERT_TRUE(LCB_MUTATION_TOKEN_ISVALID(ss));
    ASSERT_EQ(LCB_SUCCESS, rc);

    lcb_durability_opts_t options = {0};
    lcb_CMDENDURE cmd = {0};
    defaultOptions(instance2, options);
    options.version = 1;
    options.v.v0.pollopts = LCB_DURABILITY_MODE_SEQNO;

    // Initialize the command
    LCB_CMD_SET_KEY(&cmd, key.c_str(), key.size());
    cmd.mutation_token = ss;
    cmd.cmdflags |= LCB_CMDENDURE_F_MUTATION_TOKEN;

    DurabilityOperation dop;
    dop.run(instance2, &options, cmd);
    // TODO: How to actually run this?
    ASSERT_EQ(LCB_SUCCESS, dop.resp_.ctx.rc);
}

TEST_F(DurabilityUnitTest, testOptionValidation)
{
    HandleWrap hw;
    lcb_INSTANCE *instance;
    lcb_U16 persist = 0, replicate = 0;
    lcb_STATUS rc;

    createConnection(hw, &instance);

    // Validate simple mode
    persist = -1;
    replicate = -1;
    rc = lcb_durability_validate(instance, &persist, &replicate, LCB_DURABILITY_VALIDATE_CAPMAX);

    ASSERT_EQ(LCB_SUCCESS, rc);
    ASSERT_TRUE(persist > replicate);

    lcbvb_CONFIG *vbc;
    rc = lcb_cntl(instance, LCB_CNTL_GET, LCB_CNTL_VBCONFIG, &vbc);
    ASSERT_EQ(LCB_SUCCESS, rc);

    int replica_max = min(LCBVB_NREPLICAS(vbc), LCBVB_NDATASERVERS(vbc) - 1);
    int persist_max = replica_max + 1;

    ASSERT_EQ(replica_max, replicate);
    ASSERT_EQ(persist_max, persist);

    persist = 0;
    replicate = 0;
    rc = lcb_durability_validate(instance, &persist, &replicate, 0);
    ASSERT_EQ(LCB_ERR_INVALID_ARGUMENT, rc);

    persist = -1;
    replicate = -1;
    rc = lcb_durability_validate(instance, &persist, &replicate, 0);
    ASSERT_EQ(LCB_ERR_DURABILITY_TOO_MANY, rc);

    persist = persist_max;
    replicate = replica_max;
    rc = lcb_durability_validate(instance, &persist, &replicate, 0);
    ASSERT_EQ(LCB_SUCCESS, rc);
    ASSERT_EQ(persist_max, persist);
    ASSERT_EQ(replica_max, replicate);

    rc = lcb_durability_validate(instance, &persist, &replicate, LCB_DURABILITY_VALIDATE_CAPMAX);
    ASSERT_EQ(LCB_SUCCESS, rc);
    ASSERT_EQ(persist_max, persist);
    ASSERT_EQ(replica_max, replicate);
}

typedef struct {
    int store_ok;
    uint16_t npersisted;
    uint16_t nreplicated;
    lcb_STATUS rc;
} st_RESULT;

extern "C" {
static void durstoreCallback(lcb_INSTANCE *, int, const lcb_RESPSTORE *resp)
{
    st_RESULT *res;

    ASSERT_TRUE(lcb_respstore_observe_attached(resp));

    lcb_respstore_cookie(resp, (void **)&res);
    res->rc = lcb_respstore_status(resp);
    lcb_respstore_observe_stored(resp, &res->store_ok);
    lcb_respstore_observe_num_persisted(resp, &res->npersisted);
    lcb_respstore_observe_num_replicated(resp, &res->nreplicated);
}
}

TEST_F(DurabilityUnitTest, testDurStore)
{
    HandleWrap hw;
    lcb_INSTANCE *instance;
    lcb_durability_opts_t options = {0};
    createConnection(hw, &instance);
    lcb_install_callback(instance, LCB_CALLBACK_STORE, (lcb_RESPCALLBACK)durstoreCallback);

    std::string key("durStore");
    std::string value("value");

    lcb_STATUS rc;
    st_RESULT res = {0};

    lcb_CMDSTORE *cmd;
    lcb_cmdstore_create(&cmd, LCB_STORE_UPSERT);
    lcb_cmdstore_key(cmd, key.c_str(), key.size());
    lcb_cmdstore_value(cmd, value.c_str(), value.size());

    defaultOptions(instance, options);
    lcb_cmdstore_durability_observe(cmd, options.v.v0.persist_to, options.v.v0.replicate_to);
    lcb_sched_enter(instance);
    res.rc = LCB_ERR_GENERIC;
    rc = lcb_store(instance, &res, cmd);
    ASSERT_EQ(LCB_SUCCESS, rc);
    lcb_sched_leave(instance);
    lcb_wait(instance, LCB_WAIT_DEFAULT);
    lcb_cmdstore_destroy(cmd);

    ASSERT_EQ(LCB_SUCCESS, res.rc);
    ASSERT_NE(0, res.store_ok);
    ASSERT_TRUE(options.v.v0.persist_to <= res.npersisted);
    ASSERT_TRUE(options.v.v0.replicate_to <= res.nreplicated);

    lcb_cmdstore_create(&cmd, LCB_STORE_REPLACE);
    lcb_cmdstore_key(cmd, key.c_str(), key.size());
    lcb_cmdstore_value(cmd, value.c_str(), value.size());
    lcb_sched_enter(instance);
    // Try with bad criteria..
    lcb_cmdstore_durability_observe(cmd, 100, 100);
    rc = lcb_store(instance, &res, cmd);
    ASSERT_EQ(LCB_ERR_DURABILITY_TOO_MANY, rc);

    // Try with no persist/replicate options
    lcb_cmdstore_durability_observe(cmd, 0, 0);
    rc = lcb_store(instance, &res, cmd);
    ASSERT_EQ(LCB_ERR_INVALID_ARGUMENT, rc);
    lcb_sched_fail(instance);

    // CAP_MAX should be applied here
    lcb_cmdstore_durability_observe(cmd, -1, -1);
    lcb_sched_enter(instance);
    rc = lcb_store(instance, &res, cmd);
    ASSERT_EQ(LCB_SUCCESS, rc);
    lcb_sched_leave(instance);
    lcb_wait(instance, LCB_WAIT_DEFAULT);
    ASSERT_EQ(LCB_SUCCESS, res.rc);
    ASSERT_TRUE(options.v.v0.persist_to <= res.npersisted);
    ASSERT_TRUE(options.v.v0.replicate_to <= res.nreplicated);

    // Use bad CAS. we should have a clear indicator that storage failed
    lcb_cmdstore_cas(cmd, -1);
    lcb_sched_enter(instance);
    rc = lcb_store(instance, &res, cmd);
    ASSERT_EQ(LCB_SUCCESS, rc);
    lcb_sched_leave(instance);
    lcb_wait(instance, LCB_WAIT_DEFAULT);
    ASSERT_EQ(LCB_ERR_CAS_MISMATCH, res.rc);
    ASSERT_EQ(0, res.store_ok);

    // Make storage succeed, but let durability fail.
    // TODO: Add Mock-specific command to disable persistence/replication
    lcb_U32 ustmo = 1; // 1 microsecond
    rc = lcb_cntl(instance, LCB_CNTL_SET, LCB_CNTL_DURABILITY_TIMEOUT, &ustmo);
    ASSERT_EQ(LCB_SUCCESS, rc);

    // Reset CAS from previous command
    lcb_cmdstore_cas(cmd, 0);
    lcb_sched_enter(instance);
    rc = lcb_store(instance, &res, cmd);
    ASSERT_EQ(LCB_SUCCESS, rc);
    lcb_sched_leave(instance);
    lcb_wait(instance, LCB_WAIT_DEFAULT);
    if (res.rc == LCB_ERR_TIMEOUT) {
        ASSERT_NE(0, res.store_ok);
    } else {
        lcb_log(LOGARGS(instance, WARN), "Test skipped because mock is too fast(!)");
    }
    lcb_cmdstore_destroy(cmd);
}

TEST_F(DurabilityUnitTest, testFailoverAndSeqno)
{
    SKIP_UNLESS_MOCK()

    // Disable CCCP so that we get streaming updates
    MockEnvironment *mock = MockEnvironment::getInstance();
    mock->setCCCP(false);

    HandleWrap hwrap;
    lcb_INSTANCE *instance;
    lcb_durability_opts_t opts = {0};
    string key = "key-failover-seqno";
    Item itm = Item(key, key);
    KVOperation kvo(&itm);

    createConnection(hwrap);
    instance = hwrap.getLcb();

    kvo.store(instance);

    defaultOptions(instance, opts);
    DurabilityOperation dop;

    /* make sure that seqno works on healthy cluster */
    opts.version = 1;
    opts.v.v0.pollopts = LCB_DURABILITY_MODE_SEQNO;
    dop = DurabilityOperation();
    dop.run(instance, &opts, kvo.result);
    ASSERT_EQ(LCB_SUCCESS, dop.resp_.ctx.rc);

    /* failover all nodes but master */
    lcbvb_CONFIG *vbc;
    ASSERT_EQ(LCB_SUCCESS, lcb_cntl(instance, LCB_CNTL_GET, LCB_CNTL_VBCONFIG, &vbc));
    int vbid, srvix;
    lcbvb_map_key(vbc, key.c_str(), key.size(), &vbid, &srvix);
    for (size_t jj = 0; jj < lcbvb_get_nreplicas(vbc); jj++) {
        int rix = lcbvb_vbreplica(vbc, vbid, jj);
        mock->failoverNode(rix, "default", false);
    }

    /* make sure that client gets new configration */
    instance->bs_state->reset_last_refresh();
    instance->confmon->stop();
    instance->bootstrap(lcb::BS_REFRESH_ALWAYS);

    dop = DurabilityOperation();
    dop.run(instance, &opts, kvo.result);
    ASSERT_EQ(LCB_ERR_DURABILITY_TOO_MANY, dop.resp_.ctx.rc);
}

extern "C" {
struct RemoveResult {
    lcb_STATUS rc{LCB_SUCCESS};
    bool invoked{false};
};
static void removeCallback(lcb_INSTANCE *, int, const lcb_RESPREMOVE *resp)
{
    RemoveResult *res = nullptr;
    lcb_respremove_cookie(resp, (void **)&res);
    res->invoked = true;
    res->rc = lcb_respremove_status(resp);
}
}

TEST_F(DurabilityUnitTest, testRemoveWithDurability)
{
    SKIP_IF_MOCK()
    SKIP_IF_CLUSTER_VERSION_IS_LOWER_THAN(MockEnvironment::VERSION_70)

    HandleWrap hw;
    lcb_INSTANCE *instance;
    createConnection(hw, &instance);

    string key = "key-remove";
    Item itm = Item(key, key);
    KVOperation kvo(&itm);
    kvo.store(instance);

    {
        lcb_install_callback(instance, LCB_CALLBACK_REMOVE, (lcb_RESPCALLBACK)removeCallback);

        lcb_CMDREMOVE *cmd;
        lcb_cmdremove_create(&cmd);
        lcb_cmdremove_key(cmd, key.data(), key.size());
        lcb_cmdremove_durability(cmd, LCB_DURABILITYLEVEL_MAJORITY);
        RemoveResult res;
        ASSERT_EQ(LCB_SUCCESS, lcb_remove(instance, &res, cmd));
        lcb_cmdremove_destroy(cmd);
        lcb_wait(instance, LCB_WAIT_DEFAULT);
        ASSERT_TRUE(res.invoked);
        ASSERT_EQ(LCB_SUCCESS, res.rc);
    }
}
