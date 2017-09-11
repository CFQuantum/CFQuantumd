#include <BeastConfig.h>
#include <boost/algorithm/string.hpp>
#include <ripple/app/ledger/InboundLedgers.h>
#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/ledger/LedgerTiming.h>
#include <ripple/app/ledger/LedgerToJson.h>
#include <ripple/app/ledger/LocalTxs.h>
#include <ripple/app/ledger/OpenLedger.h>
#include <ripple/app/ledger/impl/DisputedTx.h>
#include <ripple/app/ledger/impl/TransactionAcquire.h>
#include <ripple/app/main/Application.h>
#include <ripple/app/misc/AmendmentTable.h>
#include <ripple/app/misc/CanonicalTXSet.h>
#include <ripple/app/misc/HashRouter.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/app/misc/TxQ.h>
#include <ripple/app/misc/Validations.h>
#include <ripple/app/tx/apply.h>
#include <ripple/basics/contract.h>
#include <ripple/basics/CountedObject.h>
#include <ripple/basics/Log.h>
#include <ripple/core/Config.h>
#include <ripple/core/ConfigSections.h>
#include <ripple/core/JobQueue.h>
#include <ripple/core/LoadFeeTrack.h>
#include <ripple/core/TimeKeeper.h>
#include <ripple/json/to_string.h>
#include <ripple/overlay/Overlay.h>
#include <ripple/overlay/predicates.h>
#include <ripple/protocol/st.h>
#include <ripple/protocol/Feature.h>
#include <ripple/unity/zookeeper.h>
#include <beast/module/core/text/LexicalCast.h>
#include <beast/utility/make_lock.h>
#include <type_traits>

namespace ripple {

std::string LedgerConsensusZk::s_hosts;
const char* LedgerConsensusZk::s_zkPath = "/" SYSTEM_NAMESPACE "/consensus";

static std::unique_ptr<ZkConnFactory> zkConnFactory;

bool shouldCloseLedger (
    bool anyTransactions,
    int previousProposers,
    int proposersClosed,
    int proposersValidated,
    int previousMSeconds,
    int currentMSeconds, // Time since last ledger's close time
    int openMSeconds,    // Time waiting to close this ledger
    int idleInterval,
    beast::Journal j);

LedgerConsensusZk::LedgerConsensusZk (
        Application& app,
        ConsensusImp& consensus,
        int previousProposers,
        int previousConvergeTime,
        InboundTransactions& inboundTransactions,
        LocalTxs& localtx,
        LedgerMaster& ledgerMaster,
        LedgerHash const & prevLCLHash,
        Ledger::ref previousLedger,
        std::uint32_t closeTime,
        FeeVote& feeVote)
    : app_ (app)
    , consensus_ (consensus)
    , inboundTransactions_ (inboundTransactions)
    , m_localTX (localtx)
    , ledgerMaster_ (ledgerMaster)
    , m_feeVote (feeVote)
    , state_ (State::open)
    , mCloseTime (closeTime)
    , mPrevLedgerHash (prevLCLHash)
    , mPreviousLedger (previousLedger)
    , mValPublic (app_.config().VALIDATION_PUB)
    , mValPrivate (app_.config().VALIDATION_PRIV)
    , mConsensusFail (false)
    , mCurrentMSeconds (0)
    , mClosePercent (0)
    , mHaveCloseTimeConsensus (false)
    , mConsensusStartTime (std::chrono::steady_clock::now ())
    , mPreviousProposers (previousProposers)
    , mPreviousMSeconds (previousConvergeTime)
    , j_ (app.journal ("LedgerConsensus"))
{
    JLOG (j_.debug) << "Creating consensus object";
    JLOG (j_.trace)
        << "LCL:" << previousLedger->getHash () << ", ct=" << closeTime;

    assert (mPreviousMSeconds);

    static bool initialized = false;
    if (!initialized)
    {
        zkConnFactory.reset (new ZkConnFactory (app.config ().section (SECTION_CONSENSUS), app.journal ("ZooKeeper")));

        // disconnect zookeeper when shutdown
        Application::signals ().Shutdown.connect (
            []() {
                zkConnFactory.reset ();
            });

        // initialize zookeeper parent path
        int ret = zoo_create (zkConnFactory->getConnection (), "/" SYSTEM_NAMESPACE, NULL, -1, &ZOO_OPEN_ACL_UNSAFE, 0, NULL, 0);
        if (ret == ZNODEEXISTS || ret == ZOK)
            ret = zoo_create (zkConnFactory->getConnection (), s_zkPath, NULL, -1, &ZOO_OPEN_ACL_UNSAFE, 0, NULL, 0);
        if (ret != ZNODEEXISTS && ret != ZOK)
        {
            JLOG (j_.error) << "Failed to create zookeeper parent path. Code " << ret;
            throw std::runtime_error ("Failed to create zookeeper parent path.");
        }
        initialized = true;
    }

    inboundTransactions_.newRound (mPreviousLedger->info().seq);

    // Adapt close time resolution to recent network conditions
    mCloseResolution = getNextLedgerTimeResolution (
        mPreviousLedger->info().closeTimeResolution,
        getCloseAgree (mPreviousLedger->info()),
        mPreviousLedger->info().seq + 1);

    if (mValPublic.isSet () && mValPrivate.isSet ()
        && !app_.getOPs ().isNeedNetworkLedger ())
    {
        // If the validation keys were set, and if we need a ledger,
        // then we want to validate, and possibly propose a ledger.
        JLOG (j_.info)
            << "Entering consensus process, validating";
        mValidating = true;
        // Propose if we are in sync with the network
        mProposing =
            app_.getOPs ().getOperatingMode () == NetworkOPs::omFULL;
    }
    else
    {
        // Otherwise we just want to monitor the validation process.
        JLOG (j_.info)
            << "Entering consensus process, watching";
        mProposing = mValidating = false;
    }

    mHaveCorrectLCL = (mPreviousLedger->getHash () == mPrevLedgerHash);

    if (!mHaveCorrectLCL)
    {
        // If we were not handed the correct LCL, then set our state
        // to not proposing.
        consensus_.setProposing (false, false);
        handleLCL (mPrevLedgerHash);

        if (!mHaveCorrectLCL)
        {
            //          mProposing = mValidating = false;
            JLOG (j_.info)
                << "Entering consensus with: "
                << previousLedger->getHash ();
            JLOG (j_.info)
                << "Correct LCL is: " << prevLCLHash;
        }
    }
    else
        // update the network status table as to whether we're
        // proposing/validating
        consensus_.setProposing (mProposing, mValidating);

    playbackProposals ();
    if (mPeerPositions.size() > (mPreviousProposers / 2))
    {
        // We may be falling behind, don't wait for the timer
        // consider closing the ledger immediately
        timerEntry ();
    }
}

Json::Value LedgerConsensusZk::getJson (bool full)
{
    Json::Value ret (Json::objectValue);
    ret["proposing"] = mProposing;
    ret["validating"] = mValidating;
    ret["proposers"] = static_cast<int> (mPeerPositions.size ());

    if (mHaveCorrectLCL)
    {
        ret["synched"] = true;
        ret["ledger_seq"] = mPreviousLedger->info().seq + 1;
        ret["close_granularity"] = mCloseResolution;
    }
    else
        ret["synched"] = false;

    switch (state_)
    {
    case State::open:
        ret[jss::state] = "open";
        break;

    case State::establish:
        ret[jss::state] = "consensus";
        break;

    case State::finished:
        ret[jss::state] = "finished";
        break;

    case State::accepted:
        ret[jss::state] = "accepted";
        break;
    }

    int v = mDisputes.size ();

    if ((v != 0) && !full)
        ret["disputes"] = v;

    if (mOurPosition)
        ret["our_position"] = mOurPosition->getJson ();

    if (full)
    {

        ret["current_ms"] = mCurrentMSeconds;
        ret["close_percent"] = mClosePercent;
        ret["close_resolution"] = mCloseResolution;
        ret["have_time_consensus"] = mHaveCloseTimeConsensus;
        ret["previous_proposers"] = mPreviousProposers;
        ret["previous_mseconds"] = mPreviousMSeconds;

        if (!mPeerPositions.empty ())
        {
            Json::Value ppj (Json::objectValue);

            for (auto& pp : mPeerPositions)
            {
                ppj[to_string (pp.first)] = pp.second->getJson ();
            }
            ret["peer_positions"] = ppj;
        }

        if (!mAcquired.empty ())
        {
            // acquired
            Json::Value acq (Json::objectValue);
            for (auto& at : mAcquired)
            {
                if (at.second)
                    acq[to_string (at.first)] = "acquired";
                else
                    acq[to_string (at.first)] = "failed";
            }
            ret["acquired"] = acq;
        }

        if (!mDisputes.empty ())
        {
            Json::Value dsj (Json::objectValue);
            for (auto& dt : mDisputes)
            {
                dsj[to_string (dt.first)] = dt.second->getJson ();
            }
            ret["disputes"] = dsj;
        }

        if (!mCloseTimes.empty ())
        {
            Json::Value ctj (Json::objectValue);
            for (auto& ct : mCloseTimes)
            {
                ctj[std::to_string(ct.first)] = ct.
                second;
            }
            ret["close_times"] = ctj;
        }

        if (!mDeadNodes.empty ())
        {
            Json::Value dnj (Json::arrayValue);
            for (auto const& dn : mDeadNodes)
            {
                dnj.append (to_string (dn));
            }
            ret["dead_nodes"] = dnj;
        }
    }

    return ret;
}

uint256 LedgerConsensusZk::getLCL ()
{
    return mPrevLedgerHash;
}

void LedgerConsensusZk::mapCompleteInternal (
    uint256 const& hash,
    std::shared_ptr<SHAMap> const& map,
    bool acquired)
{
    CondLog (acquired, lsDEBUG, LedgerConsensus)
        << "We have acquired TXS " << hash;

    if (!map)  // If the map was invalid
    {
        // this is an invalid/corrupt map
        mAcquired[hash] = map;
        JLOG (j_.warning)
            << "A trusted node directed us to acquire an invalid TXN map";
        return;
    }

    assert (hash == map->getHash ().as_uint256());

    auto it = mAcquired.find (hash);

    // If we have already acquired this transaction set
    if (it != mAcquired.end ())
    {
        if (it->second)
        {
            return; // we already have this map
        }

        // We previously failed to acquire this map, now we have it
        mAcquired.erase (hash);
    }

    // We now have a map that we did not have before

    if (!acquired)
    {
        // Put the map where others can get it
        inboundTransactions_.giveSet (hash, map, false);
    }

    // Inform directly-connected peers that we have this transaction set
    sendHaveTxSet (hash, true);

    if (mOurPosition && (!mOurPosition->isBowOut ())
        && (hash != mOurPosition->getCurrentHash ()))
    {
        // this will create disputed transactions
        auto it2 = mAcquired.find (mOurPosition->getCurrentHash ());

        if (it2 != mAcquired.end ())
        {
            assert ((it2->first == mOurPosition->getCurrentHash ())
                && it2->second);
            mCompares.insert(hash);
            // Our position is not the same as the acquired position
            createDisputes (it2->second, map);
        }
        else
            assert (false); // We don't have our own position?!
    }
    else if (!mOurPosition)
        JLOG (j_.debug)
            << "Not creating disputes: no position yet.";
    else if (mOurPosition->isBowOut ())
        JLOG (j_.warning)
            << "Not creating disputes: not participating.";
    else
        JLOG (j_.debug)
            << "Not creating disputes: identical position.";

    mAcquired[hash] = map;

    // Adjust tracking for each peer that takes this position
    std::vector<NodeID> peers;
    for (auto& it : mPeerPositions)
    {
        if (it.second->getCurrentHash () == map->getHash ().as_uint256())
            peers.push_back (it.second->getPeerID ());
    }

    if (!peers.empty ())
    {
        adjustCount (map, peers);
    }
    else
    {
        CondLog (acquired, lsWARNING, LedgerConsensus)
            << "By the time we got the map "
            << hash << " no peers were proposing it";
    }
}

void LedgerConsensusZk::mapComplete (
    uint256 const& hash,
    std::shared_ptr<SHAMap> const& map,
    bool acquired)
{
    try
    {
        mapCompleteInternal (hash, map, acquired);
    }
    catch (SHAMapMissingNode const& mn)
    {
        leaveConsensus();
        JLOG (j_.error) <<
            "Missing node processing complete map " << mn;
        Throw();
    }
}

void LedgerConsensusZk::checkLCL ()
{
    uint256 netLgr = mPrevLedgerHash;
    int netLgrCount = 0;

    uint256 favoredLedger = mPrevLedgerHash; // Don't jump forward
    uint256 priorLedger;

    if (mHaveCorrectLCL)
        priorLedger = mPreviousLedger->info().parentHash; // don't jump back

    // Get validators that are on our ledger, or  "close" to being on
    // our ledger.
    hash_map<uint256, ValidationCounter> vals =
        app_.getValidations ().getCurrentValidations(
            favoredLedger, priorLedger,
            ledgerMaster_.getValidLedgerIndex ());

    for (auto& it : vals)
    {
        if ((it.second.first > netLgrCount) ||
            ((it.second.first == netLgrCount) && (it.first == mPrevLedgerHash)))
        {
           netLgr = it.first;
           netLgrCount = it.second.first;
        }
    }

    if (netLgr != mPrevLedgerHash)
    {
        // LCL change
        const char* status;

        switch (state_)
        {
        case State::open:
            status = "open";
            break;

        case State::establish:
            status = "establish";
            break;

        case State::finished:
            status = "finished";
            break;

        case State::accepted:
            status = "accepted";
            break;

        default:
            status = "unknown";
        }

        JLOG (j_.warning)
            << "View of consensus changed during " << status
            << " (" << netLgrCount << ") status="
            << status << ", "
            << (mHaveCorrectLCL ? "CorrectLCL" : "IncorrectLCL");
        JLOG (j_.warning) << mPrevLedgerHash
            << " to " << netLgr;
        JLOG (j_.warning)
            << ripple::getJson (*mPreviousLedger);

        if (j_.debug)
        {
            for (auto& it : vals)
                j_.debug
                    << "V: " << it.first << ", " << it.second.first;
            j_.debug << getJson (true);
        }

        if (mHaveCorrectLCL)
            app_.getOPs ().consensusViewChange ();

        handleLCL (netLgr);
    }
    else if (mPreviousLedger->getHash () != mPrevLedgerHash)
        handleLCL (netLgr);
}

void LedgerConsensusZk::handleLCL (uint256 const& lclHash)
{
    assert (lclHash != mPrevLedgerHash ||
            mPreviousLedger->getHash () != lclHash);

    if (mPrevLedgerHash != lclHash)
    {
        // first time switching to this ledger
        mPrevLedgerHash = lclHash;

        if (mHaveCorrectLCL && mProposing && mOurPosition)
        {
            JLOG (j_.info) << "Bowing out of consensus";
            mOurPosition->bowOut ();
            propose ();
        }

        // Stop proposing because we are out of sync
        mProposing = false;
        mPeerPositions.clear ();
        mDisputes.clear ();
        mCloseTimes.clear ();
        mDeadNodes.clear ();
        // To get back in sync:
        playbackProposals ();
    }

    if (mPreviousLedger->getHash () == mPrevLedgerHash)
        return;

    // we need to switch the ledger we're working from
    auto newLCL = ledgerMaster_.getLedgerByHash (mPrevLedgerHash);
    if (!newLCL)
    {
        if (mAcquiringLedger != lclHash)
        {
            // need to start acquiring the correct consensus LCL
            JLOG (j_.warning) <<
                "Need consensus ledger " << mPrevLedgerHash;

            // Tell the ledger acquire system that we need the consensus ledger
            mAcquiringLedger = mPrevLedgerHash;

            auto app = &app_;
            auto hash = mAcquiringLedger;
            app_.getJobQueue().addJob (
                jtADVANCE, "getConsensusLedger",
                [app, hash] (Job&) {
                    app->getInboundLedgers().acquire(
                        hash, 0, InboundLedger::fcCONSENSUS);
                });

            mHaveCorrectLCL = false;
        }
        return;
    }

    assert (!newLCL->info().open && newLCL->isImmutable ());
    assert (newLCL->getHash () == lclHash);
    mPreviousLedger = newLCL;
    mPrevLedgerHash = lclHash;

    JLOG (j_.info) <<
        "Have the consensus ledger " << mPrevLedgerHash;
    mHaveCorrectLCL = true;

    mCloseResolution = getNextLedgerTimeResolution (
        mPreviousLedger->info().closeTimeResolution,
        getCloseAgree(mPreviousLedger->info()),
        mPreviousLedger->info().seq + 1);
}

void LedgerConsensusZk::timerEntry ()
{
    try
    {
       if ((state_ != State::finished) && (state_ != State::accepted))
            checkLCL ();

        mCurrentMSeconds = std::chrono::duration_cast<std::chrono::milliseconds>
            (std::chrono::steady_clock::now() - mConsensusStartTime).count ();
        mClosePercent = mCurrentMSeconds * 100 / mPreviousMSeconds;

        switch (state_)
        {
        case State::open:
            statePreClose ();
            return;

        case State::establish:
            stateEstablish ();

            if (state_ != State::finished) return;

            // Fall through

        case State::finished:
            stateFinished ();

            if (state_ != State::accepted) return;

            // Fall through

        case State::accepted:
            stateAccepted ();
            return;
        }

        assert (false);
    }
    catch (SHAMapMissingNode const& mn)
    {
        leaveConsensus ();
        JLOG (j_.error) <<
           "Missing node during consensus process " << mn;
        Throw();
    }
}

void LedgerConsensusZk::statePreClose ()
{
    // it is shortly before ledger close time
    bool anyTransactions = ! app_.openLedger().empty();
    int proposersClosed = mPeerPositions.size ();
    int proposersValidated
        = app_.getValidations ().getTrustedValidationCount
        (mPrevLedgerHash);

    // This computes how long since last ledger's close time
    int sinceClose;
    {
        bool previousCloseCorrect = mHaveCorrectLCL
            && getCloseAgree (mPreviousLedger->info())
            && (mPreviousLedger->info().closeTime !=
                (mPreviousLedger->info().parentCloseTime + 1));

        auto closeTime = previousCloseCorrect
            ? mPreviousLedger->info().closeTime // use consensus timing
            : consensus_.getLastCloseTime(); // use the time we saw

        auto now =
            app_.timeKeeper().closeTime().time_since_epoch().count();
        if (now >= closeTime)
            sinceClose = static_cast<int> (1000 * (now - closeTime));
        else
            sinceClose = - static_cast<int> (1000 * (closeTime - now));
    }

    auto const idleInterval = std::max (LEDGER_IDLE_INTERVAL,
        2 * mPreviousLedger->info().closeTimeResolution);

    if (shouldCloseLedger (anyTransactions
        , mPreviousProposers, proposersClosed, proposersValidated
        , mPreviousMSeconds, sinceClose, mCurrentMSeconds
        , idleInterval, app_.journal ("LedgerTiming")))
    {
        closeLedger ();
    }
    return;
    // Decide if we should close the ledger
    if ((anyTransactions &&
         mCurrentMSeconds >= LEDGER_MIN_CLOSE) ||
        sinceClose >= idleInterval * 1000)
    {
        closeLedger ();
    }
}

void LedgerConsensusZk::stateEstablish ()
{
    // Give everyone a chance to take an initial position
    if (mCurrentMSeconds < LEDGER_MIN_CONSENSUS)
        return;

    updateOurPositions ();

    // Nothing to do if we don't have consensus.
    if (!haveConsensus ())
        return;

    if (!mHaveCloseTimeConsensus)
    {
        JLOG (j_.info) <<
            "We have TX consensus but not CT consensus";
        return;
    }

    JLOG (j_.info) <<
        "Converge cutoff (" << mPeerPositions.size () << " participants)";
    state_ = State::finished;
    beginAccept (false);
}

void LedgerConsensusZk::stateFinished ()
{
    // we are processing the finished ledger
    // logic of calculating next ledger advances us out of this state
    // nothing to do
}

void LedgerConsensusZk::stateAccepted ()
{
    // we have accepted a new ledger
    endConsensus ();
}

bool LedgerConsensusZk::haveConsensus ()
{
    JLOG (j_.debug) << "Begin ZooKeeper based consensus.";

    static boost::format pathFmt = boost::format ("%s/%d"),
                         valueFmt = boost::format ("%s-%s-%d");

    auto path = boost::str (boost::format (pathFmt) % s_zkPath % (mPreviousLedger->info ().seq + 1));

    auto value = boost::str (boost::format (valueFmt) % to_string (mOurPosition->getCurrentHash ()) %
                             to_string (getLCL ()) %
                             mOurPosition->getCloseTime ());

    int ret = zoo_create (zkConnFactory->getConnection (), path.c_str (),
                          value.c_str (), value.size (),
                          &ZOO_OPEN_ACL_UNSAFE, ZOO_EPHEMERAL, NULL, 0);

    switch (ret)
    {
    case ZNODEEXISTS:
    {
        JLOG (j_.info) << "Consensus exists in ZooKeeper, check it.";
        constexpr auto buffSize = 1024;
        char buff[buffSize];
        int size = buffSize;
        Stat stat;
        ret = zoo_get (zkConnFactory->getConnection (), path.c_str (),
                       0, buff, &size, &stat);
        if (ret != ZOK || size == buffSize || size == -1)
        {
            JLOG (j_.fatal) << "zoo_get failed with size " << size
                            << " code " << ret << ", try later.";
            return false;
        }

        buff[size] = 0;
        JLOG (j_.debug) << "Consensus data: " << buff;

        std::vector<std::string> vLines;
        boost::algorithm::split (vLines, buff,
                                 boost::algorithm::is_any_of ("-"));
        if (vLines.size () < 3)
        {
            JLOG (j_.warning) << "Bad consensus data, replace it.";
            ret = zoo_set (zkConnFactory->getConnection (), path.c_str (),
                           value.c_str (), value.size (), stat.version);
            if (ret == ZOK)
            {
                JLOG (j_.info) << "Replaced in ZooKeeper.";
                mConsensusFail = false;
                return true;
            }
            JLOG (j_.warning) << "Replace failed with " << ret << ", try later.";
            return false;
        }

        uint256 txHash = from_hex_text<uint256> (vLines[0]);
        uint256 prevHash = from_hex_text<uint256> (vLines[1]);
        uint32_t closeTime = boost::lexical_cast<uint32_t> (vLines[2]);
        bool changes = false;
        if (getLCL () != prevHash)
        {
            JLOG (j_.warning) << "Previous ledger hash mismatch";
            mConsensusFail = true;
            return false;
        }

        if (mOurPosition->getCurrentHash () != txHash)
        {
            JLOG (j_.warning) << "TX hash mismatch, Our: "
                              << mOurPosition->getCurrentHash ()
                              << " published: " << txHash;
            if (mAcquired.find (txHash) == mAcquired.end ())
            {
                JLOG (j_.warning) << "TXs not acquired, try later.";
                return false;
            }
            changes = true;
        }

        if (mOurPosition->getCloseTime () != closeTime)
        {
            JLOG (j_.warning) << "Close time mismatch, Our: "
                              << mOurPosition->getCloseTime ()
                              << " published: " << closeTime;
            changes = true;
        }

        if (changes && !mOurPosition->changePosition (txHash, closeTime))
        {
            JLOG (j_.warning) << "changePosition failed, try later.";
            return false;
        }

        mConsensusFail = false;
        return true;
    }
    case ZOK:
    {
        JLOG (j_.info) << "Consensus written to ZooKeeper.";
        mConsensusFail = false;
        return true;
    }
    default:
    {
        JLOG (j_.warning) << "Create ZooKeeper node failed. Code " << ret
                          << " try later";
        return false;
    }
    }

    JLOG (j_.warning) << "Consensus failed.";
    mConsensusFail = true;
    return false;
}

std::shared_ptr<SHAMap> LedgerConsensusZk::getTransactionTree (
    uint256 const& hash)
{
    auto it = mAcquired.find (hash);
    if (it != mAcquired.end() && it->second)
        return it->second;

    auto set = inboundTransactions_.getSet (hash, true);

    if (set)
        mAcquired[hash] = set;

    return set;
}

bool LedgerConsensusZk::peerPosition (LedgerProposal::ref newPosition)
{
    auto const peerID = newPosition->getPeerID ();

    if (mDeadNodes.find (peerID) != mDeadNodes.end ())
    {
        JLOG (j_.info)
            << "Position from dead node: " << to_string (peerID);
        return false;
    }

    LedgerProposal::pointer& currentPosition = mPeerPositions[peerID];

    if (currentPosition)
    {
        assert (peerID == currentPosition->getPeerID ());

        if (newPosition->getProposeSeq ()
            <= currentPosition->getProposeSeq ())
        {
            return false;
        }
    }

    if (newPosition->isBowOut ())
    {
        JLOG (j_.info)
            << "Peer bows out: " << to_string (peerID);
        for (auto& it : mDisputes)
            it.second->unVote (peerID);
        mPeerPositions.erase (peerID);
        mDeadNodes.insert (peerID);
        return true;
    }

    if (newPosition->isInitial ())
    {
        // Record the close time estimate
        JLOG (j_.trace)
            << "Peer reports close time as "
            << newPosition->getCloseTime ();
        ++mCloseTimes[newPosition->getCloseTime ()];
    }

    JLOG (j_.trace) << "Processing peer proposal "
        << newPosition->getProposeSeq () << "/"
        << newPosition->getCurrentHash ();
    currentPosition = newPosition;

    std::shared_ptr<SHAMap> set
        = getTransactionTree (newPosition->getCurrentHash ());

    if (set)
    {
        for (auto& it : mDisputes)
            it.second->setVote (peerID, set->hasItem (it.first));
    }
    else
    {
        JLOG (j_.debug)
            << "Don't have tx set for peer";
    }

    return true;
}

void LedgerConsensusZk::simulate ()
{
    JLOG (j_.info) << "Simulating consensus";
    closeLedger ();
    mCurrentMSeconds = 100;
    beginAccept (true);
    endConsensus ();
    JLOG (j_.info) << "Simulation complete";
}

void LedgerConsensusZk::accept (std::shared_ptr<SHAMap> set)
{
    Json::Value consensusStatus;

    {
        auto lock = beast::make_lock(app_.getMasterMutex());

        // put our set where others can get it later
        if (set->getHash ().isNonZero ())
           consensus_.takePosition (mPreviousLedger->info().seq, set);

        assert (set->getHash ().as_uint256() == mOurPosition->getCurrentHash ());
        consensusStatus = getJson (true);
    }

    auto  closeTime = mOurPosition->getCloseTime ();
    bool closeTimeCorrect;

    auto replay = ledgerMaster_.releaseReplay();
    if (replay)
    {
        // replaying, use the time the ledger we're replaying closed
        closeTime = replay->closeTime_;
        closeTimeCorrect = ((replay->closeFlags_ & sLCF_NoConsensusTime) == 0);
    }
    else if (closeTime == 0)
    {
        // We agreed to disagree on the close time
        closeTime = mPreviousLedger->info().closeTime + 1;
        closeTimeCorrect = false;
    }
    else
    {
        // We agreed on a close time
        closeTime = effectiveCloseTime (closeTime);
        closeTimeCorrect = true;
    }

    JLOG (j_.debug)
        << "Report: Prop=" << (mProposing ? "yes" : "no")
        << " val=" << (mValidating ? "yes" : "no")
        << " corLCL=" << (mHaveCorrectLCL ? "yes" : "no")
        << " fail=" << (mConsensusFail ? "yes" : "no");
    JLOG (j_.debug)
        << "Report: Prev = " << mPrevLedgerHash
        << ":" << mPreviousLedger->info().seq;
    JLOG (j_.debug)
        << "Report: TxSt = " << set->getHash ()
        << ", close " << closeTime << (closeTimeCorrect ? "" : "X");

    // Put transactions into a deterministic, but unpredictable, order
    CanonicalTXSet retriableTxs (set->getHash ().as_uint256());

    // Build the new last closed ledger
    auto newLCL = std::make_shared<Ledger>(
        open_ledger, *mPreviousLedger,
        app_.timeKeeper().closeTime());
    newLCL->setClosed (); // so applyTransactions sees a closed ledger

    // Set up to write SHAMap changes to our database,
    //   perform updates, extract changes
    JLOG (j_.debug)
        << "Applying consensus set transactions to the"
        << " last closed ledger";

    {
        OpenView accum(&*newLCL);
        assert(accum.closed());
        if (replay)
        {
            // Special case, we are replaying a ledger close
            for (auto& tx : replay->txns_)
                applyTransaction (app_, accum, tx.second, false, tapNO_CHECK_SIGN, j_);
        }
        else
        {
            // Normal case, we are not replaying a ledger close
            applyTransactions (app_, set.get(), accum,
                newLCL, retriableTxs, tapNONE);
        }
        // Update fee computations.
        app_.getTxQ().processValidatedLedger(app_, accum,
            mCurrentMSeconds > 5000);

        accum.apply(*newLCL);
    }

    // retriableTxs will include any transactions that
    // made it into the consensus set but failed during application
    // to the ledger.

    newLCL->updateSkipList ();

    {
        int asf = newLCL->stateMap().flushDirty (
            hotACCOUNT_NODE, newLCL->info().seq);
        int tmf = newLCL->txMap().flushDirty (
            hotTRANSACTION_NODE, newLCL->info().seq);
        JLOG (j_.debug) << "Flushed " <<
            asf << " accounts and " <<
            tmf << " transaction nodes";
    }

    // Accept ledger
    newLCL->setAccepted (closeTime, mCloseResolution, closeTimeCorrect, app_.config());

    // And stash the ledger in the ledger master
    if (ledgerMaster_.storeLedger (newLCL))
        JLOG (j_.debug)
            << "Consensus built ledger we already had";
    else if (app_.getInboundLedgers().find (newLCL->getHash()))
        JLOG (j_.debug)
            << "Consensus built ledger we were acquiring";
    else
        JLOG (j_.debug)
            << "Consensus built new ledger";

    uint256 const newLCLHash = newLCL->getHash ();
    JLOG (j_.debug)
        << "Report: NewL  = " << newLCL->getHash ()
        << ":" << newLCL->info().seq;
    // Tell directly connected peers that we have a new LCL
    statusChange (protocol::neACCEPTED_LEDGER, *newLCL);

    if (mValidating &&
        ! ledgerMaster_.isCompatible (newLCL,
            app_.journal("LedgerConsensus").warning,
            "Not validating"))
    {
        mValidating = false;
    }

    if (mValidating && !mConsensusFail)
    {
        // Build validation
        auto v = std::make_shared<STValidation> (newLCLHash,
            consensus_.validationTimestamp (
                app_.timeKeeper().now().time_since_epoch().count()),
            mValPublic, mProposing);
        v->setFieldU32 (sfLedgerSequence, newLCL->info().seq);
        addLoad(v);  // Our network load

        if (((newLCL->info().seq + 1) % 256) == 0)
        // next ledger is flag ledger
        {
            // Suggest fee changes and new features
            m_feeVote.doValidation (newLCL, *v);
            app_.getAmendmentTable ().doValidation (newLCL, *v);
        }

        auto const signingHash = v->sign (mValPrivate);
        v->setTrusted ();
        // suppress it if we receive it - FIXME: wrong suppression
        app_.getHashRouter ().addSuppression (signingHash);
        app_.getValidations ().addValidation (v, "local");
        consensus_.setLastValidation (v);
        Blob validation = v->getSigned ();
        protocol::TMValidation val;
        val.set_validation (&validation[0], validation.size ());
        // Send signed validation to all of our directly connected peers
        app_.overlay().send(val);
        JLOG (j_.info)
            << "CNF Val " << newLCLHash;
    }
    else
        JLOG (j_.info)
            << "CNF newLCL " << newLCLHash;

    // See if we can accept a ledger as fully-validated
    ledgerMaster_.consensusBuilt (newLCL, std::move (consensusStatus));

    {
        // Apply disputed transactions that didn't get in
        //
        // The first crack of transactions to get into the new
        // open ledger goes to transactions proposed by a validator
        // we trust but not included in the consensus set.
        //
        // These are done first because they are the most likely
        // to receive agreement during consensus. They are also
        // ordered logically "sooner" than transactions not mentioned
        // in the previous consensus round.
        //
        bool anyDisputes = false;
        for (auto& it : mDisputes)
        {
            if (!it.second->getOurVote ())
            {
                // we voted NO
                try
                {
                    JLOG (j_.debug)
                        << "Test applying disputed transaction that did"
                        << " not get in";
                    SerialIter sit (it.second->peekTransaction().slice());

                    auto txn = std::make_shared<STTx const>(sit);

                    retriableTxs.insert (txn);

                    anyDisputes = true;
                }
                catch (std::exception const&)
                {
                    JLOG (j_.debug)
                        << "Failed to apply transaction we voted NO on";
                }
            }
        }

        // Build new open ledger
        auto lock = beast::make_lock(
            app_.getMasterMutex(), std::defer_lock);
        auto sl = beast::make_lock(
            ledgerMaster_.peekMutex (), std::defer_lock);
        std::lock(lock, sl);

        auto const localTx = m_localTX.getTxSet();
        auto const oldOL = ledgerMaster_.getCurrentLedger();

        auto const lastVal =
            app_.getLedgerMaster().getValidatedLedger();
        boost::optional<Rules> rules;
        if (lastVal)
            rules.emplace(*lastVal);
        else
            rules.emplace();
        app_.openLedger().accept(app_, *rules,
            newLCL, localTx, anyDisputes, retriableTxs, tapNONE,
                "consensus",
                    [&](OpenView& view, beast::Journal j)
                    {
                        // Stuff the ledger with transactions from the queue.
                        return app_.getTxQ().accept(app_, view);
                    });
    }

    mNewLedgerHash = newLCL->getHash ();
    ledgerMaster_.switchLCL (newLCL);
    state_ = State::accepted;

    assert (ledgerMaster_.getClosedLedger()->getHash() == newLCL->getHash());
    assert (app_.openLedger().current()->info().parentHash == newLCL->getHash());

    if (mValidating)
    {
        // see how close our close time is to other node's
        //  close time reports, and update our clock.
        JLOG (j_.info)
            << "We closed at " << mCloseTime;
        std::uint64_t closeTotal = mCloseTime;
        int closeCount = 1;

        for (auto it = mCloseTimes.begin ()
            , end = mCloseTimes.end (); it != end; ++it)
        {
            // FIXME: Use median, not average
            JLOG (j_.info)
                << beast::lexicalCastThrow <std::string> (it->second)
                << " time votes for "
                << beast::lexicalCastThrow <std::string> (it->first);
            closeCount += it->second;
            closeTotal += static_cast<std::uint64_t>
                (it->first) * static_cast<std::uint64_t> (it->second);
        }

        closeTotal += (closeCount / 2);
        closeTotal /= closeCount;
        int offset = static_cast<int> (closeTotal)
            - static_cast<int> (mCloseTime);
        JLOG (j_.info)
            << "Our close offset is estimated at "
            << offset << " (" << closeCount << ")";
        app_.timeKeeper().adjustCloseTime(
            std::chrono::seconds(offset));
    }
}

void LedgerConsensusZk::createDisputes (
    std::shared_ptr<SHAMap> const& m1,
    std::shared_ptr<SHAMap> const& m2)
{
    if (m1->getHash() == m2->getHash())
        return;

    JLOG (j_.debug) << "createDisputes "
        << m1->getHash() << " to " << m2->getHash();
    SHAMap::Delta differences;
    m1->compare (*m2, differences, 16384);

    int dc = 0;
    // for each difference between the transactions
    for (auto& pos : differences)
    {
        ++dc;
        // create disputed transactions (from the ledger that has them)
        if (pos.second.first)
        {
            // transaction is only in first map
            assert (!pos.second.second);
            addDisputedTransaction (pos.first
                , pos.second.first->peekData ());
        }
        else if (pos.second.second)
        {
            // transaction is only in second map
            assert (!pos.second.first);
            addDisputedTransaction (pos.first
                , pos.second.second->peekData ());
        }
        else // No other disagreement over a transaction should be possible
            assert (false);
    }
    JLOG (j_.debug) << dc << " differences found";
}

void LedgerConsensusZk::addDisputedTransaction (
    uint256 const& txID,
    Blob const& tx)
{
    if (mDisputes.find (txID) != mDisputes.end ())
        return;

    JLOG (j_.debug) << "Transaction "
        << txID << " is disputed";

    bool ourVote = false;

    // Update our vote on the disputed transaction
    if (mOurPosition)
    {
        auto mit (mAcquired.find (mOurPosition->getCurrentHash ()));

        if (mit != mAcquired.end ())
            ourVote = mit->second->hasItem (txID);
        else
            assert (false); // We don't have our own position?
    }

    auto txn = std::make_shared<DisputedTx> (txID, tx, ourVote, j_);
    mDisputes[txID] = txn;

    // Update all of the peer's votes on the disputed transaction
    for (auto& pit : mPeerPositions)
    {
        auto cit (mAcquired.find (pit.second->getCurrentHash ()));

        if ((cit != mAcquired.end ()) && cit->second)
        {
            txn->setVote (pit.first, cit->second->hasItem (txID));
        }
    }

    // If we didn't relay this transaction recently, relay it
    if (app_.getHashRouter ().setFlags (txID, SF_RELAYED))
    {
        protocol::TMTransaction msg;
        msg.set_rawtransaction (& (tx.front ()), tx.size ());
        msg.set_status (protocol::tsNEW);
        msg.set_receivetimestamp (
            app_.timeKeeper().now().time_since_epoch().count());
        app_.overlay ().foreach (send_always (
            std::make_shared<Message> (
                msg, protocol::mtTRANSACTION)));
    }
}

void LedgerConsensusZk::adjustCount (std::shared_ptr<SHAMap> const& map,
                  const std::vector<NodeID>& peers)
{
    for (auto& it : mDisputes)
    {
        bool setHas = map->hasItem (it.second->getTransactionID ());
        for (auto const& pit : peers)
            it.second->setVote (pit, setHas);
    }
}

void LedgerConsensusZk::leaveConsensus ()
{
    if (mProposing)
    {
        if (mOurPosition && ! mOurPosition->isBowOut ())
        {
            mOurPosition->bowOut();
            propose();
        }
        mProposing = false;
    }
}

void LedgerConsensusZk::propose ()
{
    JLOG (j_.trace) << "We propose: " <<
        (mOurPosition->isBowOut ()
            ? std::string ("bowOut")
            : to_string (mOurPosition->getCurrentHash ()));
    protocol::TMProposeSet prop;

    prop.set_currenttxhash (mOurPosition->getCurrentHash ().begin ()
        , 256 / 8);
    prop.set_previousledger (mOurPosition->getPrevLedger ().begin ()
        , 256 / 8);
    prop.set_proposeseq (mOurPosition->getProposeSeq ());
    prop.set_closetime (mOurPosition->getCloseTime ());

    Blob const pubKey = mValPublic.getNodePublic ();
    prop.set_nodepubkey (&pubKey[0], pubKey.size ());

    Blob const sig = mOurPosition->sign (mValPrivate);
    prop.set_signature (&sig[0], sig.size ());

    app_.overlay().send(prop);
}

void LedgerConsensusZk::sendHaveTxSet (uint256 const& hash, bool direct)
{
    protocol::TMHaveTransactionSet msg;
    msg.set_hash (hash.begin (), 256 / 8);
    msg.set_status (direct ? protocol::tsHAVE : protocol::tsCAN_GET);
    app_.overlay ().foreach (send_always (
        std::make_shared <Message> (
            msg, protocol::mtHAVE_SET)));
}

void LedgerConsensusZk::statusChange (
    protocol::NodeEvent event, Ledger& ledger)
{
    protocol::TMStatusChange s;

    if (!mHaveCorrectLCL)
        s.set_newevent (protocol::neLOST_SYNC);
    else
        s.set_newevent (event);

    s.set_ledgerseq (ledger.info().seq);
    s.set_networktime (app_.timeKeeper().now().time_since_epoch().count());
    s.set_ledgerhashprevious(ledger.info().parentHash.begin (),
        std::decay_t<decltype(ledger.info().parentHash)>::bytes);
    s.set_ledgerhash (ledger.getHash ().begin (),
        std::decay_t<decltype(ledger.getHash ())>::bytes);

    std::uint32_t uMin, uMax;
    if (!ledgerMaster_.getFullValidatedRange (uMin, uMax))
    {
        uMin = 0;
        uMax = 0;
    }
    else
    {
        // Don't advertise ledgers we're not willing to serve
        std::uint32_t early = ledgerMaster_.getEarliestFetch ();
        if (uMin < early)
           uMin = early;
    }
    s.set_firstseq (uMin);
    s.set_lastseq (uMax);
    app_.overlay ().foreach (send_always (
        std::make_shared <Message> (
            s, protocol::mtSTATUS_CHANGE)));
    JLOG (j_.trace) << "send status change to peer";
}

void LedgerConsensusZk::takeInitialPosition (
    std::shared_ptr<ReadView const> const& initialLedger)
{
    std::shared_ptr<SHAMap> initialSet = std::make_shared <SHAMap> (
        SHAMapType::TRANSACTION, app_.family());

    // Build SHAMap containing all transactions in our open ledger
    for (auto const& tx : initialLedger->txs)
    {
        Serializer s (2048);
        tx.first->add(s);
        initialSet->addItem (
            SHAMapItem (tx.first->getTransactionID(), std::move (s)), true, false);
    }

    if ((app_.config().RUN_STANDALONE || (mProposing && mHaveCorrectLCL))
            && ((mPreviousLedger->info().seq % 256) == 0))
    {
        // previous ledger was flag ledger, add pseudo-transactions
        ValidationSet parentSet = app_.getValidations().getValidations (
            mPreviousLedger->info().parentHash);
        m_feeVote.doVoting (mPreviousLedger, parentSet, initialSet);
        app_.getAmendmentTable ().doVoting (
            mPreviousLedger, parentSet, initialSet);
    }

    // Set should be immutable snapshot
    initialSet = initialSet->snapShot (false);

    // Tell the ledger master not to acquire the ledger we're probably building
    ledgerMaster_.setBuildingLedger (mPreviousLedger->info().seq + 1);

    auto txSet = initialSet->getHash ().as_uint256();
    JLOG (j_.info) << "initial position " << txSet;
    mapCompleteInternal (txSet, initialSet, false);

    mOurPosition = std::make_shared<LedgerProposal>
        (mValPublic, initialLedger->info().parentHash, txSet, mCloseTime);

    for (auto& it : mDisputes)
    {
        it.second->setOurVote (initialLedger->txExists (it.first));
    }

    // if any peers have taken a contrary position, process disputes
    hash_set<uint256> found;

    for (auto& it : mPeerPositions)
    {
        uint256 set = it.second->getCurrentHash ();

        if (found.insert (set).second)
        {
            auto iit (mAcquired.find (set));

            if (iit != mAcquired.end ())
            {
                mCompares.insert(iit->second->getHash().as_uint256());
                createDisputes (initialSet, iit->second);
            }
        }
    }

    if (mProposing)
        propose ();
}

std::uint32_t LedgerConsensusZk::effectiveCloseTime (std::uint32_t closeTime)
{
    if (closeTime == 0)
        return 0;

    return std::max (
        roundCloseTime (closeTime, mCloseResolution),
        mPreviousLedger->info().closeTime + 1);
}

void LedgerConsensusZk::updateOurPositions ()
{
    // Do not check CloseTime when using zk consensus.
    mHaveCloseTimeConsensus = true;
}

void LedgerConsensusZk::playbackProposals ()
{
    for (auto const& it: consensus_.peekStoredProposals ())
    {
        for (auto const& proposal : it.second)
        {
            if (proposal->isPrevLedger (mPrevLedgerHash) &&
                peerPosition (proposal))
            {
                JLOG (j_.warning)
                    << "We should do delayed relay of this proposal,"
                    << " but we cannot";
            }
        }
    }
}

void LedgerConsensusZk::closeLedger ()
{
    checkOurValidation ();
    state_ = State::establish;
    mConsensusStartTime = std::chrono::steady_clock::now ();
    mCloseTime = app_.timeKeeper().closeTime().time_since_epoch().count();
    consensus_.setLastCloseTime (mCloseTime);
    statusChange (protocol::neCLOSING_LEDGER, *mPreviousLedger);
    ledgerMaster_.applyHeldTransactions ();
    takeInitialPosition (app_.openLedger().current());
}

void LedgerConsensusZk::checkOurValidation ()
{
    // This only covers some cases - Fix for the case where we can't ever
    // acquire the consensus ledger
    if (!mHaveCorrectLCL || !mValPublic.isSet ()
        || !mValPrivate.isSet ()
        || app_.getOPs ().isNeedNetworkLedger ())
    {
        return;
    }

    auto lastValidation = consensus_.getLastValidation ();

    if (lastValidation)
    {
        if (lastValidation->getFieldU32 (sfLedgerSequence)
            == mPreviousLedger->info().seq)
        {
            return;
        }
        if (lastValidation->getLedgerHash () == mPrevLedgerHash)
            return;
    }

    auto v = std::make_shared<STValidation> (mPreviousLedger->getHash (),
        consensus_.validationTimestamp (
            app_.timeKeeper().now().time_since_epoch().count()),
        mValPublic, false);
    addLoad(v);
    v->setTrusted ();
    auto const signingHash = v->sign (mValPrivate);
        // FIXME: wrong supression
    app_.getHashRouter ().addSuppression (signingHash);
    app_.getValidations ().addValidation (v, "localMissing");
    Blob validation = v->getSigned ();
    protocol::TMValidation val;
    val.set_validation (&validation[0], validation.size ());
    consensus_.setLastValidation (v);
    JLOG (j_.warning) << "Sending partial validation";
}

void LedgerConsensusZk::beginAccept (bool synchronous)
{
    auto consensusSet = mAcquired[mOurPosition->getCurrentHash ()];

    if (!consensusSet)
    {
        JLOG (j_.fatal)
            << "We don't have a consensus set";
        abort ();
        return;
    }

    consensus_.newLCL (
        mPeerPositions.size (), mCurrentMSeconds, mNewLedgerHash);

    if (synchronous)
        accept (consensusSet);
    else
    {
        app_.getJobQueue().addJob (jtACCEPT, "acceptLedger",
            std::bind (&LedgerConsensusZk::accept, shared_from_this (),
                       consensusSet));
    }
}

void LedgerConsensusZk::endConsensus ()
{
    app_.getOPs ().endConsensus (mHaveCorrectLCL);
}

void LedgerConsensusZk::addLoad(STValidation::ref val)
{
    std::uint32_t fee = std::max(
        app_.getFeeTrack().getLocalFee(),
        app_.getFeeTrack().getClusterFee());
    std::uint32_t ref = app_.getFeeTrack().getLoadBase();
    if (fee > ref)
        val->setFieldU32(sfLoadFee, fee);
}

//------------------------------------------------------------------------------
std::shared_ptr <LedgerConsensus>
make_LedgerConsensusZk (Application& app, ConsensusImp& consensus, int previousProposers,
    int previousConvergeTime, InboundTransactions& inboundTransactions,
    LocalTxs& localtx, LedgerMaster& ledgerMaster,
    LedgerHash const &prevLCLHash,
    Ledger::ref previousLedger, std::uint32_t closeTime, FeeVote& feeVote)
{
    return std::make_shared <LedgerConsensusZk> (app, consensus, previousProposers,
        previousConvergeTime, inboundTransactions, localtx, ledgerMaster,
        prevLCLHash, previousLedger, closeTime, feeVote);
}

} // ripple
