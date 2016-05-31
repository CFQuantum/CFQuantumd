#if (defined (_WIN32) || defined (_WIN64))
#include <Psapi.h>
#else
#include <sys/time.h>
#include <sys/resource.h>
#endif
#include <boost/multiprecision/cpp_int.hpp>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/depth_first_search.hpp>

#include <beast/threads/RecursiveMutex.h>

#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/main/Application.h>
#include <ripple/app/misc/DividendMaster.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/basics/Log.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/SystemParameters.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/json/to_string.h>
#include <ripple/server/Role.h>
#include <ripple/rpc/impl/AccountFromString.h>
#include <ripple/rpc/impl/TransactionSign.h>
#include <ripple/thrift/HBaseConn.h>
#include <ripple/core/ConfigSections.h>

namespace ripple {
        
static inline uint64_t memUsed(void) {
#if (defined (_WIN32) || defined (_WIN64))
    //CARL windows implementation
    HANDLE hProcess = GetCurrentProcess();
    PROCESS_MEMORY_COUNTERS pmc;
    BOOL bOk = GetProcessMemoryInfo(hProcess, &pmc, sizeof(pmc));
    return pmc.WorkingSetSize / (1024 * 1024);
#else
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss/1024;
#endif
}
    
class DividendMasterImpl : public DividendMaster
{
public:
    DividendMasterImpl (Application& app, beast::Journal journal)
        : app_ (app)
        , m_journal (journal)
        , m_hbaseFactory(app.config ().section (SECTION_QUANTUM), journal)
    {
        initTables ();
    }
    
    QuantumDividend& getDivResult()
    {
        return m_divQuantumResult;
    }
    
    QuantumDividend& getQuantumDivResult ()
    {
        return m_divQuantumResult;
    }
    
    int getDividendState() override
    {
        return m_dividendState;
    }
    
    void setDividendState(int dividendState) override
    {
        m_dividendState = dividendState;
    }
    
    HBaseConn* getConnection ()
    {
        return m_hbaseFactory.getConnection ();
    }
    
    void initTables ()
    {
        using namespace apache::thrift;
        using namespace apache::hadoop::hbase::thrift;
        
        std::vector<ColumnDescriptor> columns;
        columns.push_back (ColumnDescriptor ());
        columns.back ().name = s_columnFamily;
        columns.back ().maxVersions = 1;
        columns.back ().compression = "SNAPPY";
        columns.back ().blockCacheEnabled = true;
        columns.back ().bloomFilterType = "ROW";

        try
        {
            getConnection ()->m_client->createTable (s_tableTxns, columns);
        }
        catch (const AlreadyExists& ae)
        {
            JLOG (m_journal.debug) << "Table " << s_tableTxns << " exists, " << ae.message;
        }
        catch (const TException& te)
        {
            JLOG (m_journal.error) << "Create table " << s_tableTxns << " failed, " << te.what ();
            throw std::runtime_error (te.what ());
        }
    }

    bool calcQuantumDividend (const uint32_t ledgerIndex)  override
    {
        Ledger::pointer ledger = app_.getLedgerMaster ().getLedgerBySeq (ledgerIndex);
        LedgerInfo const& info = ledger->info ();
        if (!ledger)
        {
            return false;
        }
        
        std::unordered_map<AccountID, std::shared_ptr<QuantumData>> accounts;
        accounts.clear ();
        uint32_t now = info.closeTime;
        uint64_t sumEnergy = 0;
        uint32_t accountsCounter = 0;

        auto accountVisitor = [&](SLE::ref sle)
        {
            if (sle->getType () != ltACCOUNT_ROOT)
                return;
                
            accountsCounter++;
            
            auto account = sle->getAccountID (sfAccount);
            auto balance = sle->getFieldAmount (sfBalance).mantissa ();
            if (balance < XRS_DIVIDEND_MIN)
            {
                JLOG (m_journal.debug) << "Account: " << account << " passed, balance " << balance <<" less than 1";
                return;
            }
            auto iter = accounts.find(account);
            bool result;
            if (iter == accounts.end ())
            {
                std::tie (iter, result) = accounts.emplace (account, std::make_shared<QuantumData>(account, balance));
            }
            uint32_t linksCount = sle->getFieldU32 (sfQuantumLinksCount);
            if (linksCount == 0)
                return;
            
            std::vector<std::shared_ptr<SLE const>> items;
            forEachItem (*ledger, keylet::quantumDir (account),
                [&items](std::shared_ptr<SLE const> const&sleCur)
                {
                    if (sleCur)
                        items.emplace_back(sleCur);
            });
            
            // set up account's children,parent
            uint64_t activity = 0;
            for (auto const& item : items)
            {
                AccountID highAccountID = item->getAccountID (sfHighAccount);
                AccountID lowAccountID = item->getAccountID (sfLowAccount);
                uint32_t lowWeight = item->getFieldU32 (sfQuantumLowWeight);
                uint32_t highWeight = item->getFieldU32 (sfQuantumHighWeight);
                
                bool bLow = account == lowAccountID ? true : false;
                auto const opAccount = bLow ? highAccountID : lowAccountID;
                uint32_t weight = bLow ? lowWeight : highWeight;
                uint32_t opWeight = bLow ? highWeight : lowWeight;
                
                uint64_t opBalance = ledger->read(keylet::account (
                    bLow ? highAccountID : lowAccountID))->getFieldAmount (sfBalance).mantissa ();
                if (opBalance < XRS_DIVIDEND_MIN)
                {
                    JLOG (m_journal.debug) << "Child account: " << opAccount << " passed, balance " << opBalance <<" less than 1";
                    continue;
                }
                if (weight == 1)
                {
                    iter->second->parent = opAccount;
                }
                else if (opWeight == 1)
                {
                    //iter->second->references->emplace (opAccount);
                }
                
                if ((bLow && lowWeight > highWeight) || (!bLow && highWeight > lowWeight))
                {
                    iter->second->children.emplace (opAccount, std::tuple<uint64_t>(weight));
                    JLOG (m_journal.debug) << "Add child:" << opAccount << " weight:" << weight <<" for account:" << account;
                }
                else
                {
                    //
                }
                
                // calc activity
                uint32_t refresh = item->getFieldU32 (bLow ? sfQuantumLowRefresh : sfQuantumHighRefresh);
                int daysPassed = (now - refresh) / (24*60*60);
                if (daysPassed >= 7)
                    continue;


                activity += pow (0.5, daysPassed) * opBalance;
                
                JLOG (m_journal.debug) << "Account:" << account << " passedDays:" << daysPassed << " balance:"<< opBalance << " activity:" << activity;
            }
            //
            iter->second->activity = activity;
            iter->second->linksCount = linksCount;

        };
        ledger->visitStateItems (accountVisitor);
        m_quantumDivTotalAccounts = accountsCounter;
        JLOG (m_journal.info) << "accounts size: " << accounts.size ();

        // calc tree transfer energy
        std::vector<AccountID> stack;
        AccountID root;
        RPC::accountFromString (root, "cDop6BbtxA5SmGahAtM741Ruf6cwke67MY", true);
        auto iter = accounts.find (root);
        while (iter != accounts.end () || !stack.empty ())
        {
            while (iter != accounts.end () && !iter->second->children.empty () && !iter->second->bVisited)
            {
                for (auto it=iter->second->children.begin (); it!=iter->second->children.end(); it++)
                {
                    stack.push_back (it->first);
                }

                iter->second->bVisited = true;
                iter = accounts.find (stack.back ());
            }

            // transfer balance+energyT to parent
            auto parent = accounts.find (iter->second->parent);
            parent->second->energyT += iter->second->balance + iter->second->energyT;
            stack.pop_back ();
            
            // next one
            iter = accounts.find (stack.back ());
        }

        // calc collect energy
        auto accountVisitor1 = [&](SLE::ref sle)
        {
            if (sle->getType () != ltACCOUNT_ROOT)
                return;
            auto account = sle->getAccountID (sfAccount);
            auto iter = accounts.find (account);
            if (iter == accounts.end ())
                return;
            double e = 2.71828;
            double energyC = 0;
            double childEnergy, cchildEnergy, weight;
            std::vector<AccountID> bans;
            for (auto child : iter->second->children)
            {
                auto iterChild = accounts.find (child.first);
                for (auto cchild : iterChild->second->children)
                {
                    JLOG (m_journal.debug) << "    --calc cchild's energy collection:" << cchild.first;
                    auto iterCChild = accounts.find (cchild.first);
                    
                    if (iterCChild->second->children.empty ())
                    {
                        JLOG (m_journal.debug) << "    cchild:" << iterCChild->first << " does not have any child.";
                        continue;
                    }
                    weight = std::get<0> (iterChild->second->children.find (iterCChild->first)->second);
                    if (weight > 5 || weight <= 0)
                    {
                        JLOG (m_journal.debug) << "    cchild:" << iterCChild->first << " weight:" <<weight<< " < 1/5 passed.";
                    }
                    cchildEnergy = pow(1/weight, e) * iterCChild->second->balance;
                    if (cchildEnergy < 1)
                        continue;
                    energyC += cchildEnergy;
                }
                weight = std::get<0>(iter->second->children.find (child.first)->second);
                childEnergy = (1 / weight) * iter->second->balance;
                if (childEnergy > 1)
                    energyC += childEnergy;
            }
            iter->second->energyC = energyC;
            
            uint64_t energyT = iter->second->energyT;
            uint64_t energy = energyT + energyC;
            uint64_t accountEnergy = (energy + iter->second->balance) * log(iter->second->activity + iter->second->balance);
            sumEnergy += accountEnergy;
            iter->second->energy = accountEnergy;
            
            JLOG (m_journal.debug) << "Calc account:" << account << " collect energy:" << energyC << " transfer energy:" << energyT << " final energy:" << accountEnergy;
        };
        ledger->visitStateItems (accountVisitor1);
        
        // calc total dividend coins
        calcDividendCoins (ledger);
        
        m_quantumDivTotalEnergy = sumEnergy;
        m_divQuantumResult.clear ();
        double ratio = float(m_quantumDivTotalCoins) / float(m_quantumDivTotalEnergy);
        for (auto account : accounts)
        {
            account.second->divAmount = account.second->energy * ratio;
            
            // balance, divCoins, Activity, energy, links,
            std::pair<QuantumDividend::iterator, bool> ret = m_divQuantumResult.emplace (std::piecewise_construct, 
                std::forward_as_tuple (account.first), 
                std::forward_as_tuple (account.second->balance, account.second->divAmount, account.second->activity, account.second->energy, account.second->linksCount));
            if (ret.second == false)
            {
                JLOG (m_journal.warning) << "Insert same account: " << account.first << " into dividend result map error!";
            }
            JLOG (m_journal.debug) << "Dividend result: account:" << account.first << " diviendAmount:" << account.second->divAmount 
                << " balance:" << account.second->balance << " energy:" << account.second->energy 
                << " activity:" << account.second->activity << " links:" <<account.second->linksCount;
        }

        JLOG (m_journal.info) << "Dividend calculate finished, sumDividiend:" << m_quantumDivTotalCoins << " sumEnergy:" << m_quantumDivTotalEnergy << " sumDivAccounts:" << m_quantumDivTotalAccounts;
        
        return true;
    }

    void calcDividendCoins (Ledger::pointer const ledger)
    {
        double rate = get<double> (app_.config ()[SECTION_QUANTUM], "dividend_rate");
        if (rate > 0)
        {
            m_quantumDivTotalCoins = ledger->info ().drops.drops () * rate;
            JLOG (m_journal.debug) << "Generate " << m_quantumDivTotalCoins << " coins by dividend_rate:" << rate;
            return;
        }
        auto divObj = ledger->read (keylet::dividend ());
        if (!divObj)
        {
            // first day divCoins
            m_quantumDivTotalCoins = DIVIDEND_INITIAL_RATIO * SYSTEM_CURRENCY_START;
            return;
        }
        uint64_t lastTimeDivCoins = divObj->getFieldU64 (sfQuantumCoins);
        uint64_t lastDayAccounts = divObj->getFieldU64 (sfQuantumAccounts);
        
        uint32_t lastDivLedgerIndex = divObj->getFieldU32 (sfDividendLedger);
        auto const lastDivLedger = app_.getLedgerMaster ().getLedgerBySeq (lastDivLedgerIndex);
        uint64_t lastDayTotalCoins = lastDivLedger->info ().drops.drops ();

        if (lastTimeDivCoins == 0 || lastDayAccounts == 0 || lastDivLedgerIndex == 0 || lastDayTotalCoins == 0)
        {
            m_quantumDivTotalCoins = DIVIDEND_INITIAL_RATIO * SYSTEM_CURRENCY_START;
            return;
        }

        double coinsIncreaseRatio = lastTimeDivCoins / lastDayTotalCoins;
        double accountsIncreaseRatio = lastDayAccounts / m_quantumDivTotalAccounts;
        
        uint64_t divCoins = coinsIncreaseRatio < accountsIncreaseRatio ? 
            lastTimeDivCoins * (1 + accountsIncreaseRatio) :
            lastTimeDivCoins * (1 + accountsIncreaseRatio - coinsIncreaseRatio);
        
        m_quantumDivTotalCoins = divCoins;
        JLOG (m_journal.debug) << "LastDayDivCoins:" << lastTimeDivCoins << " lastDayTotalCoins:" << lastDayTotalCoins << " coinsIncRatio:" << coinsIncreaseRatio
            << " lastDayAccounts:" << lastDayAccounts << " Accounts:" << m_quantumDivTotalAccounts << " accountIncRatio:" << accountsIncreaseRatio
            << " divCoins:" << divCoins;
    };

    bool dumpQuantumDividend (const uint32_t ledgerIndex) override
    {
        using namespace apache::thrift;
        using namespace apache::hadoop::hbase::thrift;
        
        std::string secret_key = get<std::string> (app_.config ()[SECTION_QUANTUM], "secret_key");
        RippleAddress secret = RippleAddress::createSeedGeneric (secret_key);
        RippleAddress generator = RippleAddress::createGeneratorPublic (secret);
        RippleAddress naAccountPrivate = RippleAddress::createAccountPrivate (generator, secret, 0);
        RippleAddress accountPublic = RippleAddress::createAccountPublic (generator, 0);
        
        // TODO
        // format date from ledgerTime
        char buffer[10];
        time_t now;
        time (&now);
        struct tm* now_gmt = gmtime (&now);
        strftime(buffer, sizeof(buffer), "%Y%m%d", now_gmt);
        std::string date = std::string (buffer);

        m_divTxns.clear ();
        std::vector<BatchMutation> batches;
        for (auto const& div : m_divQuantumResult)
        {
            // make transaction <Balance, DivCoins, Activity, Energy, Links>
            STTx trans (ttISSUE);
            trans.setFieldU8 (sfDividendType, DividendMaster::DivType_Apply);
            trans.setFieldU32 (sfDividendLedger, ledgerIndex);
            trans.setFieldU32 (sfFlags, tfFullyCanonicalSig);
            trans.setAccountID (sfAccount, AccountID ());
            trans.setAccountID (sfDestination, div.first);
            trans.setFieldU64 (sfQuantumCoins, std::get<1> (div.second));
            trans.setFieldU64 (sfQuantumActivity, std::get<2> (div.second));
            trans.setFieldU64 (sfQuantumEnergy, std::get<3> (div.second));
            trans.setFieldVL (sfSigningPubKey, accountPublic.getAccountPublic ());
            
            uint256 txID = trans.getHash(HashPrefix::transactionID);
            Serializer s;
            trans.add (s);
            
            m_divTxns.emplace (div.first, std::make_shared<STTx>(trans));
            
            // write txns
            boost::format s_keyTxns = boost::format ("%s-%X"); // YYYYMMDD-AccountID
            std::string s_columnRaw = s_columnName;
            std::string const rowKey (boost::str (s_keyTxns % date % div.first));
            
            batches.push_back (BatchMutation ());
            batches.back ().row = rowKey;
            
            auto& mutations = batches.back ().mutations;
            mutations.push_back(Mutation ());
            mutations.back ().column = s_columnRaw;
            mutations.back ().value.assign (s.getString ());
        }
        
        JLOG (m_journal.debug) << "Going to write " << batches.size () << " txns into hbase.";
        for (int i = 0; i < 3; i++)
        {
            try
            {
                std::map<Text, Text> attributes;
                getConnection ()->m_client->mutateRows (
                    s_tableTxns, batches, attributes);
                JLOG (m_journal.info) << "save tx done";
            }
            catch (const TException& te)
            {
                JLOG (m_journal.error) << "save TX failed, " << te.what ();
                if (i == 2)
                {
                    JLOG (m_journal.error) << "fail to save quantum dividend txns";
                    return false;
                }
            }
        }
        return true;
    };
    
    bool launchDividend (const uint32_t ledgerIndex) override
    {
        std::string secret_key = get<std::string> (app_.config ()[SECTION_QUANTUM], "secret_key");

        RippleAddress secret = RippleAddress::createSeedGeneric (secret_key);
        RippleAddress generator = RippleAddress::createGeneratorPublic (secret);
        RippleAddress naAccountPrivate = RippleAddress::createAccountPrivate (generator, secret, 0);
        RippleAddress accountPublic = RippleAddress::createAccountPublic (generator, 0);

        std::shared_ptr<STTx> trans = std::make_shared<STTx> (ttISSUE);
        trans->setFieldU32 (sfDividendLedger, ledgerIndex);
        trans->setFieldU8 (sfDividendType, DividendMaster::DivType_Start);
        trans->setFieldU32 (sfFlags, tfFullyCanonicalSig);
        trans->setAccountID (sfAccount, AccountID());
        trans->setAccountID (sfDestination, AccountID());
        trans->setFieldU64 (sfQuantumCoins, m_quantumDivTotalCoins);
        trans->setFieldU64 (sfQuantumEnergy, m_quantumDivTotalEnergy);
        trans->setFieldU64 (sfQuantumAccounts, m_quantumDivTotalAccounts);
        trans->setFieldVL (sfSigningPubKey, accountPublic.getAccountPublic ());

        trans->sign (naAccountPrivate);

        app_.getJobQueue ().addJob (
            jtTRANSACTION, "launchDividend",
            std::bind (&NetworkOPs::submitTransaction, &app_.getOPs (),
                       trans));

        JLOG(m_journal.info) << "Launch dividend,dividend state " << getDividendState ();
        return true;
    };
    
    void dividendProgress() override
    {
        using namespace apache::thrift;
        using namespace apache::hadoop::hbase::thrift;
        if (app_.getOPs ().isNeedNetworkLedger ())
        {
            return;
        }
        
        beast::Journal journal(app_.journal("DividendMaster"));
        auto const& curLedger = app_.getLedgerMaster ().getCurrentLedger ();
        auto const& dividendObj = curLedger->read (keylet::dividend ());
        if (!dividendObj)
            return;

        std::string secret_key = get<std::string> (app_.config ()[SECTION_QUANTUM], "secret_key");
        RippleAddress secret = RippleAddress::createSeedGeneric (secret_key);
        RippleAddress generator = RippleAddress::createGeneratorPublic (secret);
        RippleAddress const& naAccountPrivate = RippleAddress::createAccountPrivate (generator, secret, 0);

        if (m_divTxns.empty () && getDividendState () == DividendMaster::DivType_Start)
        {
            // fetch data from hbase
            try
            {
                std::map<Text, Text> attributes;
                std::vector<Text> columns;
                boost::format prefix ("%s-");
                
                char buffer[10];
                time_t now;
                time (&now);
                struct tm* now_gmt = gmtime (&now);
                strftime(buffer, sizeof(buffer), "%Y%m%d", now_gmt);
                std::string date = std::string (buffer);
                
                auto scanner = getConnection ()->m_client->scannerOpenWithPrefix (
                    s_tableTxns, boost::str (prefix % date), columns, attributes);
                
                std::vector<TRowResult> rowList;
                for (;;)
                {
                    getConnection ()->m_client->scannerGetList (rowList, scanner, 100);
                    if (rowList.empty ())
                        break;
                    
                    for (auto& row : rowList)
                    {
                        auto& columns = row.columns;
                        if (columns.find (s_columnName) == columns.end ())
                        {
                            if (m_journal.fatal)
                                m_journal.fatal << "column not found for quantum dividend txns #" << row.row;
                            continue;
                        }

                        auto& data = columns[s_columnName].value;
                        std::vector<std::string> fields;
                        boost::split (fields, row.row, boost::is_any_of ("-"));
                        AccountID account;
                        RPC::accountFromString (account, fields[1], true);
                        SerialIter txn (data.data (), data.size ());
                        
                        m_divTxns.emplace (account, std::make_shared<STTx> (txn));
                        JLOG (m_journal.debug) << "Got quantum dividend txn account:" << account << " from hbase";
                    }
                }

                getConnection ()->m_client->scannerClose (scanner);
            }
            catch (const TException& te)
            {
                JLOG (m_journal.error) << "fetch quantum dividend txns failed, " << te.what ();
            }
        }
        
        int shots = 200;
        int passes = 1;
        
        AccountID marker = dividendObj->getAccountID (sfDividendMarker);
        auto txnIter = m_divTxns.find (marker);
        AccountID lastAccount = txnIter->first;
        if (txnIter != m_divTxns.end ())
            txnIter++;
        uint32_t dividendLedger = dividendObj->getFieldU32 (sfDividendLedger);
        std::unordered_map<AccountID, int> submitted;
        while (shots > 0 && getDividendState () == DividendMaster::DivType_Start)
        {
            if (txnIter == m_divTxns.end ())
            {
                txnIter = m_divTxns.begin ();
            }
                
            auto item = txnIter->second;
            AccountID destAccount = item->getAccountID (sfDestination);
            
            JLOG (journal.debug) << "Dividend job, prev account:" << lastAccount;
            JLOG (journal.debug) << "Dividend job, this account:" << destAccount;
            txnIter++;
            if (submitted.find (destAccount) != submitted.end ())
            {
                JLOG (m_journal.trace) << "Duplicate txn account:" << destAccount<<" submitted";
                shots--;
                continue;
            }
            auto accountSLE = curLedger->read (keylet::account (destAccount));
            if (accountSLE->getFieldU32 (sfDividendLedger) == dividendLedger)
            {
                if (lastAccount == destAccount)
                {
                    setDividendState(DividendMaster::DivType_Done);
                    JLOG(journal.debug) << "Dividend job, finish last txn " << lastAccount;
                    break;
                }
                JLOG(journal.debug) << "Dividend job, account: "<< destAccount << " dividend ledger: " << accountSLE->getFieldU32 (sfDividendLedger);
                JLOG(journal.debug) << "Dividend job, "<< passes << " pass applied transaction " << destAccount;
                passes++;
                //shots--;
                continue;
            }
            try
            {
                shots--;
                lastAccount = destAccount;
                std::shared_ptr<STTx> stpTrans = item;
                stpTrans->sign (naAccountPrivate);
                JLOG(journal.debug) << "Dividend job, submit tx " << lastAccount << " with signed tx id " << destAccount << " shots:" << shots;
                app_.getOPs ().submitTransaction (stpTrans);
                submitted.emplace (destAccount, 1);
            }
            catch (std::runtime_error& e)
            {
                JLOG(journal.debug) << "Duplicate transaction applied" << e.what ();
            }
        }
    };

private:
    Application& app_;
    beast::Journal m_journal;
    
    //int m_dividendState = DivType_Start;
    int m_dividendState = DivType_Done;
    
    HBaseConnFactory m_hbaseFactory;
    std::string s_tableTxns = "Cf:DivTxns";
    std::string s_columnFamily = "q:";
    std::string s_columnName = "q:r";
    
    std::map<AccountID, std::shared_ptr<STTx>> m_divTxns;
    QuantumDividend m_divQuantumResult;
    uint64_t m_quantumDivTotalCoins;
    uint64_t m_quantumDivTotalAccounts;
    uint64_t m_quantumDivTotalEnergy;
    
    class QuantumData
    {
    public:
        QuantumData (const AccountID& accountId, uint64_t balance)
            : account (accountId)
            , balance (balance) 
            {}
            
        AccountID account;
        AccountID parent;

        // children weight,
        std::map<AccountID, std::tuple<uint64_t>> children;
        //std::vector<AccountID> references;
        
        int linksCount = 0;
        bool bVisited = false;
        uint64_t balance = 0;
        uint64_t energy = 0;
        uint64_t energyC = 0;   // collect
        uint64_t energyT = 0;   // transfer
        uint64_t activity = 0;
        uint64_t divAmount = 0;
    };
};

std::unique_ptr<DividendMaster>
make_DividendMaster(Application& app, beast::Journal journal)
{
    return std::make_unique<DividendMasterImpl>(app, journal);
}

}
