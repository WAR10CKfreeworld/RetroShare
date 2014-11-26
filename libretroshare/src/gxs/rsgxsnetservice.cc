
/*
 * libretroshare/src/gxs: rsgxnetservice.cc
 *
 * Access to rs network and synchronisation service implementation
 *
 * Copyright 2012-2012 by Christopher Evi-Parker
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License Version 2 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA.
 *
 * Please report all bugs and problems to "retroshare@lunamutt.com".
 *
 */

#include <unistd.h>
#include <sys/time.h>
#include <math.h>

#include "rsgxsnetservice.h"
#include "retroshare/rsconfig.h"
#include "retroshare/rsgxsflags.h"
#include "retroshare/rsgxscircles.h"
#include "pgp/pgpauxutils.h"

/***
 * #define NXS_NET_DEBUG	1
 ***/

#define GIXS_CUT_OFF 0

#define SYNC_PERIOD     60 // every 60 seconds (12 seconds for testing)
#define TRANSAC_TIMEOUT 30 // 30 seconds. Has been increased to avoid epidemic transaction cancelling due to overloaded outqueues.

 const uint32_t RsGxsNetService::FRAGMENT_SIZE = 150000;

RsGxsNetService::RsGxsNetService(uint16_t servType, RsGeneralDataService *gds,
                                 RsNxsNetMgr *netMgr, RsNxsObserver *nxsObs, 
				const RsServiceInfo serviceInfo,
				RsGixsReputation* reputations, RsGcxs* circles, 
				PgpAuxUtils *pgpUtils, bool grpAutoSync)
                                     : p3ThreadedService(), p3Config(), mTransactionN(0),
                                       mObserver(nxsObs), mDataStore(gds), mServType(servType),
                                       mTransactionTimeOut(TRANSAC_TIMEOUT), mNetMgr(netMgr), mNxsMutex("RsGxsNetService"),
                                       mSyncTs(0), mLastKeyPublishTs(0), mSYNC_PERIOD(SYNC_PERIOD), mCircles(circles), mReputations(reputations),
					mPgpUtils(pgpUtils),
					mGrpAutoSync(grpAutoSync), mGrpServerUpdateItem(NULL),
					mServiceInfo(serviceInfo)

{
	addSerialType(new RsNxsSerialiser(mServType));
	mOwnId = mNetMgr->getOwnId();
}

RsGxsNetService::~RsGxsNetService()
{
    RS_STACK_MUTEX(mNxsMutex) ;

    for(TransactionsPeerMap::iterator it = mTransactions.begin();it!=mTransactions.end();++it)
    {
        for(TransactionIdMap::iterator it2 = it->second.begin();it2!=it->second.end();++it2)
            delete it2->second ;

        it->second.clear() ;
    }
    mTransactions.clear() ;
}


int RsGxsNetService::tick()
{
	// always check for new items arriving
	// from peers
    if(receivedItems())
        recvNxsItemQueue();

    time_t now = time(NULL);
    time_t elapsed = mSYNC_PERIOD + mSyncTs;

    if((elapsed) < now)
    {
        syncWithPeers();
    	mSyncTs = now;
    }

    if(now > 10 + mLastKeyPublishTs)
    {
        sharePublishKeysPending() ;
        mLastKeyPublishTs = now ;
    }

    return 1;
}

// This class collects outgoing items due to the broadcast of Nxs messages. It computes
// a probability that can be used to temper the broadcast of items so as to match the
// residual bandwidth (difference between max allowed bandwidth and current outgoing rate.

class NxsBandwidthRecorder
{
public:
    static const int OUTQUEUE_CUTOFF_VALUE = 500 ;
        static const int BANDWIDTH_ESTIMATE_DELAY = 20 ;

    static void recordEvent(uint16_t service_type, RsItem *item)
    {
		  RS_STACK_MUTEX(mtx) ;

        uint32_t bw = RsNxsSerialiser(service_type).size(item) ;	// this is used to estimate bandwidth.
        timeval tv ;
        gettimeofday(&tv,NULL) ;

        // compute time(NULL) in msecs, for a more accurate bw estimate.

        uint64_t now = tv.tv_sec * 1000 + tv.tv_usec/1000 ;

        total_record += bw ;
        ++total_events ;

#ifdef NXS_NET_DEBUG
        std::cerr << "bandwidthRecorder::recordEvent() Recording event time=" << now << ". bw=" << bw << std::endl;
#endif

        // Every 20 seconds at min, compute a new estimate of the required bandwidth.

        if(now > last_event_record + BANDWIDTH_ESTIMATE_DELAY*1000)
        {
            // Compute the bandwidth using recorded times, in msecs
            float speed = total_record/1024.0f/(now - last_event_record)*1000.0f ;

            // Apply a small temporal convolution.
            estimated_required_bandwidth = 0.75*estimated_required_bandwidth + 0.25 * speed ;

#ifdef NXS_NET_DEBUG
            std::cerr << std::dec << "  " << total_record << " Bytes (" << total_events << " items)"
                      << " received in " << now - last_event_record << " seconds. Speed: " << speed << " KBytes/sec" << std::endl;
            std::cerr << "  instantaneous speed = " << speed << " KB/s" << std::endl;
            std::cerr << "  cumulated estimated = " << estimated_required_bandwidth << " KB/s" << std::endl;
#endif

            last_event_record = now ;
            total_record = 0 ;
            total_events = 0 ;
        }
    }

    // Estimate the probability of sending an item so that the expected bandwidth matches the residual bandwidth

    static float computeCurrentSendingProbability()
    {
        int maxIn=50,maxOut=50;
        float currIn=0,currOut=0 ;

        rsConfig->GetMaxDataRates(maxIn,maxOut) ;
        rsConfig->GetCurrentDataRates(currIn,currOut) ;

    RsConfigDataRates rates ;
    rsConfig->getTotalBandwidthRates(rates) ;

    std::cerr << std::dec << std::endl;

    float outqueue_factor     = 1.0f/pow( std::max(0.02f,rates.mQueueOut / (float)OUTQUEUE_CUTOFF_VALUE),5.0f) ;
    float accepted_bandwidth  = std::max( 0.0f, maxOut - currOut) ;
    float max_bandwidth_factor = std::min( accepted_bandwidth / estimated_required_bandwidth,1.0f ) ;

    // We account for two things here:
    //   1 - the required max bandwidth
    //   2 - the current network overload, measured from the size of the outqueues.
    //
    // Only the later can limit the traffic if the internet connexion speed is responsible for outqueue overloading.

    float sending_probability = std::min(outqueue_factor,max_bandwidth_factor) ;

#ifdef NXS_NET_DEBUG
        std::cerr << "bandwidthRecorder::computeCurrentSendingProbability()" << std::endl;
        std::cerr << "  current required bandwidth : " << estimated_required_bandwidth << " KB/s" << std::endl;
    std::cerr << "  max_bandwidth_factor       : " << max_bandwidth_factor << std::endl;
    std::cerr << "  outqueue size              : " << rates.mQueueOut << ", factor=" << outqueue_factor << std::endl;
        std::cerr << "  max out                    : " << maxOut << ", currOut=" << currOut << std::endl;
        std::cerr << "  computed probability       : " << sending_probability << std::endl;
#endif

        return sending_probability ;
    }

private:
    static RsMutex mtx;
    static uint64_t last_event_record ;
    static float estimated_required_bandwidth ;
    static uint32_t total_events ;
    static uint64_t total_record ;
};

uint32_t NxsBandwidthRecorder::total_events =0 ;		  // total number of events. Not used.
uint64_t NxsBandwidthRecorder::last_event_record = time(NULL) * 1000;// starting time of bw estimate period (in msec)
uint64_t NxsBandwidthRecorder::total_record =0 ;		  // total bytes recorded in the current time frame
float    NxsBandwidthRecorder::estimated_required_bandwidth = 10.0f ;// Estimated BW for sending sync data. Set to 10KB/s, to avoid 0.
RsMutex  NxsBandwidthRecorder::mtx("Bandwidth recorder") ; 	  // Protects the recorder since bw events are collected from multiple GXS Net services

void RsGxsNetService::syncWithPeers()
{
#ifdef NXS_NET_DEBUG
	std::cerr << "RsGxsNetService::syncWithPeers()";
	std::cerr << std::endl;
#endif

    static RsNxsSerialiser ser(mServType) ;	// this is used to estimate bandwidth.

	RS_STACK_MUTEX(mNxsMutex) ;

	std::set<RsPeerId> peers;
	mNetMgr->getOnlineList(mServiceInfo.mServiceType, peers);

	std::set<RsPeerId>::iterator sit = peers.begin();

	if(mGrpAutoSync)
	{
		// for now just grps
		for(; sit != peers.end(); ++sit)
        {

            const RsPeerId peerId = *sit;

            ClientGrpMap::const_iterator cit = mClientGrpUpdateMap.find(peerId);
            uint32_t updateTS = 0;
            if(cit != mClientGrpUpdateMap.end())
            {
                const RsGxsGrpUpdateItem *gui = cit->second;
                updateTS = gui->grpUpdateTS;
            }
            RsNxsSyncGrp *grp = new RsNxsSyncGrp(mServType);
            grp->clear();
            grp->PeerId(*sit);
            grp->updateTS = updateTS;

            NxsBandwidthRecorder::recordEvent(mServType,grp) ;

            sendItem(grp);
        }
    }

#ifndef GXS_DISABLE_SYNC_MSGS

        typedef std::map<RsGxsGroupId, RsGxsGrpMetaData* > GrpMetaMap;
        GrpMetaMap grpMeta;

        mDataStore->retrieveGxsGrpMetaData(grpMeta);

        GrpMetaMap::iterator
                mit = grpMeta.begin();

        GrpMetaMap toRequest;

        for(; mit != grpMeta.end(); ++mit)
        {
            RsGxsGrpMetaData* meta = mit->second;

            if(meta->mSubscribeFlags & GXS_SERV::GROUP_SUBSCRIBE_SUBSCRIBED )
            {
				toRequest.insert(std::make_pair(mit->first, meta));
            }else
            	delete meta;
        }

        grpMeta.clear();

        sit = peers.begin();

    float sending_probability = NxsBandwidthRecorder::computeCurrentSendingProbability() ;
#ifdef NXS_NET_DEBUG
    std::cerr << "syncWithPeers(): Sending probability = " << sending_probability << std::endl;
#endif

        // synchronise group msg for groups which we're subscribed to
        for(; sit != peers.end(); ++sit)
        {
            const RsPeerId& peerId = *sit;


            // now see if you have an updateTS so optimise whether you need
            // to get a new list of peer data
            RsGxsMsgUpdateItem* mui = NULL;

            ClientMsgMap::const_iterator cit = mClientMsgUpdateMap.find(peerId);

            if(cit != mClientMsgUpdateMap.end())
            {
                mui = cit->second;
            }

            GrpMetaMap::const_iterator mmit = toRequest.begin();
            for(; mmit != toRequest.end(); ++mmit)
            {
            	const RsGxsGrpMetaData* meta = mmit->second;
            	const RsGxsGroupId& grpId = mmit->first;

            	if(!checkCanRecvMsgFromPeer(peerId, *meta))
            		continue;

                uint32_t updateTS = 0;
                if(mui)
                {
                    std::map<RsGxsGroupId, uint32_t>::const_iterator cit2 =
                    		mui->msgUpdateTS.find(grpId);

                    if(cit2 != mui->msgUpdateTS.end())
                    {
                        updateTS = cit2->second;
                    }
                }

                RsNxsSyncMsg* msg = new RsNxsSyncMsg(mServType);
                msg->clear();
                msg->PeerId(peerId);
                msg->grpId = grpId;
                msg->updateTS = updateTS;

        NxsBandwidthRecorder::recordEvent(mServType,msg) ;

        if(RSRandom::random_f32() < sending_probability)
            sendItem(msg);
        else
            delete msg ;
        }
        }

        GrpMetaMap::iterator mmit = toRequest.begin();
        for(; mmit != toRequest.end(); ++mmit)
        {
        	delete mmit->second;
        }
#endif
}


bool RsGxsNetService::fragmentMsg(RsNxsMsg& msg, MsgFragments& msgFragments) const
{
	// first determine how many fragments
	uint32_t msgSize = msg.msg.TlvSize();
	uint32_t dataLeft = msgSize;
	uint8_t nFragments = ceil(float(msgSize)/FRAGMENT_SIZE);
	char buffer[FRAGMENT_SIZE];
	int currPos = 0;


	for(uint8_t i=0; i < nFragments; ++i)
	{
		RsNxsMsg* msgFrag = new RsNxsMsg(mServType);
		msgFrag->grpId = msg.grpId;
		msgFrag->msgId = msg.msgId;
		msgFrag->meta = msg.meta;
		msgFrag->transactionNumber = msg.transactionNumber;
		msgFrag->pos = i;
		msgFrag->PeerId(msg.PeerId());
		msgFrag->count = nFragments;
		uint32_t fragSize = std::min(dataLeft, FRAGMENT_SIZE);

		memcpy(buffer, ((char*)msg.msg.bin_data) + currPos, fragSize);
		msgFrag->msg.setBinData(buffer, fragSize);

		currPos += fragSize;
		dataLeft -= fragSize;
		msgFragments.push_back(msgFrag);
	}

	return true;
}

bool RsGxsNetService::fragmentGrp(RsNxsGrp& grp, GrpFragments& grpFragments) const
{
	// first determine how many fragments
	uint32_t grpSize = grp.grp.TlvSize();
	uint32_t dataLeft = grpSize;
	uint8_t nFragments = ceil(float(grpSize)/FRAGMENT_SIZE);
	char buffer[FRAGMENT_SIZE];
	int currPos = 0;


	for(uint8_t i=0; i < nFragments; ++i)
	{
		RsNxsGrp* grpFrag = new RsNxsGrp(mServType);
		grpFrag->grpId = grp.grpId;
		grpFrag->meta = grp.meta;
		grpFrag->pos = i;
		grpFrag->count = nFragments;
		uint32_t fragSize = std::min(dataLeft, FRAGMENT_SIZE);

		memcpy(buffer, ((char*)grp.grp.bin_data) + currPos, fragSize);
		grpFrag->grp.setBinData(buffer, fragSize);

		currPos += fragSize;
		dataLeft -= fragSize;
		grpFragments.push_back(grpFrag);
	}

	return true;
}

RsNxsMsg* RsGxsNetService::deFragmentMsg(MsgFragments& msgFragments) const
{
	if(msgFragments.empty()) return NULL;

	// if there is only one fragment with a count 1 or less then
	// the fragment is the msg
	if(msgFragments.size() == 1)
	{
		RsNxsMsg* m  = msgFragments.front();
		if(m->count > 1)
			return NULL;
		else
			return m;
	}

	// first determine total size for binary data
	MsgFragments::iterator mit = msgFragments.begin();
	uint32_t datSize = 0;

	for(; mit != msgFragments.end(); ++mit)
		datSize += (*mit)->msg.bin_len;

	char* data = new char[datSize];
	uint32_t currPos = 0;

	for(mit = msgFragments.begin(); mit != msgFragments.end(); ++mit)
	{
		RsNxsMsg* msg = *mit;
		memcpy(data + (currPos), msg->msg.bin_data, msg->msg.bin_len);
		currPos += msg->msg.bin_len;
	}

	RsNxsMsg* msg = new RsNxsMsg(mServType);
	const RsNxsMsg& m = *(*(msgFragments.begin()));
	msg->msg.setBinData(data, datSize);
	msg->msgId = m.msgId;
	msg->grpId = m.grpId;
	msg->transactionNumber = m.transactionNumber;
	msg->meta = m.meta;

	delete[] data;
	return msg;
}

RsNxsGrp* RsGxsNetService::deFragmentGrp(GrpFragments& grpFragments) const
{
	if(grpFragments.empty()) return NULL;

	// first determine total size for binary data
	GrpFragments::iterator mit = grpFragments.begin();
	uint32_t datSize = 0;

	for(; mit != grpFragments.end(); ++mit)
		datSize += (*mit)->grp.bin_len;

	char* data = new char[datSize];
	uint32_t currPos = 0;

	for(mit = grpFragments.begin(); mit != grpFragments.end(); ++mit)
	{
		RsNxsGrp* grp = *mit;
		memcpy(data + (currPos), grp->grp.bin_data, grp->grp.bin_len);
		currPos += grp->grp.bin_len;
	}

	RsNxsGrp* grp = new RsNxsGrp(mServType);
	const RsNxsGrp& g = *(*(grpFragments.begin()));
	grp->grp.setBinData(data, datSize);
	grp->grpId = g.grpId;
	grp->transactionNumber = g.transactionNumber;
	grp->meta = g.meta;

	delete[] data;

	return grp;
}

struct GrpFragCollate
{
	RsGxsGroupId mGrpId;
	GrpFragCollate(const RsGxsGroupId& grpId) : mGrpId(grpId){ }
	bool operator()(RsNxsGrp* grp) { return grp->grpId == mGrpId;}
};

void RsGxsNetService::locked_createTransactionFromPending(
		MsgRespPending* msgPend)
{
	MsgAuthorV::const_iterator cit = msgPend->mMsgAuthV.begin();
	std::list<RsNxsItem*> reqList;
	uint32_t transN = locked_getTransactionId();
	for(; cit != msgPend->mMsgAuthV.end(); ++cit)
	{
		const MsgAuthEntry& entry = *cit;

		if(entry.mPassedVetting)
		{
			RsNxsSyncMsgItem* msgItem = new RsNxsSyncMsgItem(mServType);
			msgItem->grpId = entry.mGrpId;
			msgItem->msgId = entry.mMsgId;
			msgItem->authorId = entry.mAuthorId;
			msgItem->flag = RsNxsSyncMsgItem::FLAG_REQUEST;
			msgItem->transactionNumber = transN;
			msgItem->PeerId(msgPend->mPeerId);
			reqList.push_back(msgItem);
		}
	}

	if(!reqList.empty())
		locked_pushMsgTransactionFromList(reqList, msgPend->mPeerId, transN);
}

void RsGxsNetService::locked_createTransactionFromPending(
		GrpRespPending* grpPend)
{
	GrpAuthorV::const_iterator cit = grpPend->mGrpAuthV.begin();
	std::list<RsNxsItem*> reqList;
	uint32_t transN = locked_getTransactionId();
	for(; cit != grpPend->mGrpAuthV.end(); ++cit)
	{
		const GrpAuthEntry& entry = *cit;

		if(entry.mPassedVetting)
		{
#ifdef NXS_NET_DEBUG
			std::cerr << "locked_createTransactionFromPending(AUTHOR VETTING) Group Id: " << entry.mGrpId << " PASSED";
			std::cerr << std::endl;
#endif
			RsNxsSyncGrpItem* msgItem = new RsNxsSyncGrpItem(mServType);
			msgItem->grpId = entry.mGrpId;
			msgItem->authorId = entry.mAuthorId;
			msgItem->flag = RsNxsSyncMsgItem::FLAG_REQUEST;
			msgItem->transactionNumber = transN;
			msgItem->PeerId(grpPend->mPeerId);
			reqList.push_back(msgItem);
		}
		else
		{
#ifdef NXS_NET_DEBUG
			std::cerr << "locked_createTransactionFromPending(AUTHOR VETTING) Group Id: " << entry.mGrpId << " FAILED";
			std::cerr << std::endl;
#endif
		}
	}

	if(!reqList.empty())
		locked_pushGrpTransactionFromList(reqList, grpPend->mPeerId, transN);
}


void RsGxsNetService::locked_createTransactionFromPending(GrpCircleIdRequestVetting* grpPend)
{
	std::vector<GrpIdCircleVet>::iterator cit = grpPend->mGrpCircleV.begin();
	uint32_t transN = locked_getTransactionId();
	std::list<RsNxsItem*> itemL;
	for(; cit != grpPend->mGrpCircleV.end(); ++cit)
	{
		const GrpIdCircleVet& entry = *cit;
		if(entry.mCleared)
		{
#ifdef NXS_NET_DEBUG
			std::cerr << "locked_createTransactionFromPending(CIRCLE VETTING) Group Id: " << entry.mGroupId << " PASSED";
			std::cerr << std::endl;
#endif
			RsNxsSyncGrpItem* gItem = new
			RsNxsSyncGrpItem(mServType);
			gItem->flag = RsNxsSyncGrpItem::FLAG_RESPONSE;
			gItem->grpId = entry.mGroupId;
			gItem->publishTs = 0;
			gItem->PeerId(grpPend->mPeerId);
			gItem->transactionNumber = transN;
			gItem->authorId = entry.mAuthorId;
			// why it authorId not set here???
			itemL.push_back(gItem);
		}
		else
		{
#ifdef NXS_NET_DEBUG
			std::cerr << "locked_createTransactionFromPending(CIRCLE VETTING) Group Id: " << entry.mGroupId << " FAILED";
			std::cerr << std::endl;
#endif
		}
	}

	if(!itemL.empty())
		locked_pushGrpRespFromList(itemL, grpPend->mPeerId, transN);
}

void RsGxsNetService::locked_createTransactionFromPending(MsgCircleIdsRequestVetting* msgPend)
{
	std::vector<MsgIdCircleVet>::iterator vit = msgPend->mMsgs.begin();
	std::list<RsNxsItem*> itemL;

	uint32_t transN = locked_getTransactionId();

	for(; vit != msgPend->mMsgs.end(); ++vit)
	{
		MsgIdCircleVet& mic = *vit;
		RsNxsSyncMsgItem* mItem = new
		RsNxsSyncMsgItem(mServType);
		mItem->flag = RsNxsSyncGrpItem::FLAG_RESPONSE;
		mItem->grpId = msgPend->mGrpId;
		mItem->msgId = mic.mMsgId;
		mItem->authorId = mic.mAuthorId;
		mItem->PeerId(msgPend->mPeerId);
		mItem->transactionNumber =  transN;
		itemL.push_back(mItem);
	}

	if(!itemL.empty())
		locked_pushMsgRespFromList(itemL, msgPend->mPeerId, transN);
}

/*bool RsGxsNetService::locked_canReceive(const RsGxsGrpMetaData * const grpMeta
                                      , const RsPeerId& peerId )
{

	double timeDelta = 0.2;

	if(grpMeta->mCircleType == GXS_CIRCLE_TYPE_EXTERNAL) {
		int i=0;
		mCircles->loadCircle(grpMeta->mCircleId);

		// check 5 times at most
		// spin for 1 second at most
		while(i < 5) {

			if(mCircles->isLoaded(grpMeta->mCircleId)) {
				const RsPgpId& pgpId = mPgpUtils->getPGPId(peerId);
				return mCircles->canSend(grpMeta->mCircleId, pgpId);
			}//if(mCircles->isLoaded(grpMeta->mCircleId))

			usleep((int) (timeDelta * 1000 * 1000));// timeDelta sec
			i++;
		}//while(i < 5)

	} else {//if(grpMeta->mCircleType == GXS_CIRCLE_TYPE_EXTERNAL)
		return true;
	}//else (grpMeta->mCircleType == GXS_CIRCLE_TYPE_EXTERNAL)

	return false;
}*/

void RsGxsNetService::collateGrpFragments(GrpFragments fragments,
		std::map<RsGxsGroupId, GrpFragments>& partFragments) const
{
	// get all unique grpIds;
	GrpFragments::iterator vit = fragments.begin();
	std::set<RsGxsGroupId> grpIds;

	for(; vit != fragments.end(); ++vit)
		grpIds.insert( (*vit)->grpId );

	std::set<RsGxsGroupId>::iterator sit = grpIds.begin();

	for(; sit != grpIds.end(); ++sit)
	{
		const RsGxsGroupId& grpId = *sit;
		GrpFragments::iterator bound = std::partition(
					fragments.begin(), fragments.end(),
					GrpFragCollate(grpId));

		// something will always be found for a group id
		for(vit = fragments.begin(); vit != bound; )
		{
			partFragments[grpId].push_back(*vit);
			vit = fragments.erase(vit);
		}

		GrpFragments& f = partFragments[grpId];
		RsNxsGrp* grp = *(f.begin());

		// if counts of fragments is incorrect remove
		// from coalescion
		if(grp->count != f.size())
		{
			GrpFragments::iterator vit2 = f.begin();

			for(; vit2 != f.end(); ++vit2)
				delete *vit2;

			partFragments.erase(grpId);
		}
	}

	fragments.clear();
}

struct MsgFragCollate
{
	RsGxsMessageId mMsgId;
	MsgFragCollate(const RsGxsMessageId& msgId) : mMsgId(msgId){ }
	bool operator()(RsNxsMsg* msg) { return msg->msgId == mMsgId;}
};

void RsGxsNetService::collateMsgFragments(MsgFragments fragments, std::map<RsGxsMessageId, MsgFragments>& partFragments) const
{
	// get all unique message Ids;
	MsgFragments::iterator vit = fragments.begin();
	std::set<RsGxsMessageId> msgIds;

	for(; vit != fragments.end(); ++vit)
		msgIds.insert( (*vit)->msgId );


	std::set<RsGxsMessageId>::iterator sit = msgIds.begin();

	for(; sit != msgIds.end(); ++sit)
	{
		const RsGxsMessageId& msgId = *sit;
		MsgFragments::iterator bound = std::partition(
					fragments.begin(), fragments.end(),
					MsgFragCollate(msgId));

		// something will always be found for a group id
		for(vit = fragments.begin(); vit != bound; ++vit )
		{
			partFragments[msgId].push_back(*vit);
		}

		fragments.erase(fragments.begin(), bound);
		MsgFragments& f = partFragments[msgId];
		RsNxsMsg* msg = *(f.begin());

		// if counts of fragments is incorrect remove
		// from coalescion
		if(msg->count != f.size())
		{
			MsgFragments::iterator vit2 = f.begin();

			for(; vit2 != f.end(); ++vit2)
				delete *vit2;

			partFragments.erase(msgId);
		}
	}

	fragments.clear();
}

class StoreHere
{
public:

    StoreHere(RsGxsNetService::ClientGrpMap& cgm, RsGxsNetService::ClientMsgMap& cmm,
              RsGxsNetService::ServerMsgMap& smm,
              RsGxsServerGrpUpdateItem*& sgm) : mClientGrpMap(cgm), mClientMsgMap(cmm),
    mServerMsgMap(smm), mServerGrpUpdateItem(sgm)
    {}

    void operator() (RsItem* item)
    {
        RsGxsMsgUpdateItem* mui;
        RsGxsGrpUpdateItem* gui;
        RsGxsServerGrpUpdateItem* gsui;
        RsGxsServerMsgUpdateItem* msui;

        if((mui = dynamic_cast<RsGxsMsgUpdateItem*>(item)) != NULL)
            mClientMsgMap.insert(std::make_pair(mui->peerId, mui));
        else if((gui = dynamic_cast<RsGxsGrpUpdateItem*>(item)) != NULL)
            mClientGrpMap.insert(std::make_pair(gui->peerId, gui));
        else if((msui = dynamic_cast<RsGxsServerMsgUpdateItem*>(item)) != NULL)
            mServerMsgMap.insert(std::make_pair(msui->grpId, msui));
        else if((gsui = dynamic_cast<RsGxsServerGrpUpdateItem*>(item)) != NULL)
        {
            if(mServerGrpUpdateItem == NULL)
            {
                mServerGrpUpdateItem = gsui;
            }
            else
            {
#ifdef NXS_NET_DEBUG
                std::cerr << "Error! More than one server group update item exists!" << std::endl;
#endif
                delete gsui;
            }
        }
        else
        {
            std::cerr << "Type not expected!" << std::endl;
        }

    }

private:

    RsGxsNetService::ClientGrpMap& mClientGrpMap;
    RsGxsNetService::ClientMsgMap& mClientMsgMap;
    RsGxsNetService::ServerMsgMap& mServerMsgMap;
    RsGxsServerGrpUpdateItem*& mServerGrpUpdateItem;

};

bool RsGxsNetService::loadList(std::list<RsItem *> &load)
{
    std::for_each(load.begin(), load.end(), StoreHere(mClientGrpUpdateMap, mClientMsgUpdateMap,
                                                      mServerMsgUpdateMap, mGrpServerUpdateItem));

    return true;
}

#include <algorithm>

template <typename UpdateMap>
struct get_second : public std::unary_function<typename UpdateMap::value_type, RsItem*>
{
    RsItem* operator()(const typename UpdateMap::value_type& value) const
    {
        return value.second;
    }
};

bool RsGxsNetService::saveList(bool& cleanup, std::list<RsItem*>& save)
{
	RS_STACK_MUTEX(mNxsMutex) ;

    // hardcore templates
    std::transform(mClientGrpUpdateMap.begin(), mClientGrpUpdateMap.end(),
                   std::back_inserter(save), get_second<ClientGrpMap>());

    std::transform(mClientMsgUpdateMap.begin(), mClientMsgUpdateMap.end(),
                   std::back_inserter(save), get_second<ClientMsgMap>());

    std::transform(mServerMsgUpdateMap.begin(), mServerMsgUpdateMap.end(),
                   std::back_inserter(save), get_second<ServerMsgMap>());

    save.push_back(mGrpServerUpdateItem);

    cleanup = false;
    return true;
}

RsSerialiser *RsGxsNetService::setupSerialiser()
{

    RsSerialiser *rss = new RsSerialiser;
    rss->addSerialType(new RsGxsUpdateSerialiser(mServType));

    return rss;
}

void RsGxsNetService::recvNxsItemQueue(){

	RsItem *item ;

	while(NULL != (item=recvItem()))
	{
#ifdef NXS_NET_DEBUG
		std::cerr << "RsGxsNetService Item:" << (void*)item << std::endl ;
		item->print(std::cerr);
#endif
		// RsNxsItem needs dynamic_cast, since they have derived siblings.
		//
		RsNxsItem *ni = dynamic_cast<RsNxsItem*>(item) ;
		if(ni != NULL)
		{
			// a live transaction has a non zero value
			if(ni->transactionNumber != 0){

#ifdef NXS_NET_DEBUG
				std::cerr << "recvNxsItemQueue()" << std::endl;
				std::cerr << "handlingTransaction, transN" << ni->transactionNumber << std::endl;
#endif

                if(!handleTransaction(ni))
                    delete ni;

                continue;
			}


			switch(ni->PacketSubType())
			{
				case RS_PKT_SUBTYPE_NXS_SYNC_GRP: handleRecvSyncGroup (dynamic_cast<RsNxsSyncGrp*>(ni)) ; break ;
				case RS_PKT_SUBTYPE_NXS_SYNC_MSG: handleRecvSyncMessage (dynamic_cast<RsNxsSyncMsg*>(ni)) ; break ;
                case RS_PKT_SUBTYPE_NXS_GRP_PUBLISH_KEY: handleRecvPublishKeys (dynamic_cast<RsNxsGroupPublishKeyItem*>(ni)) ; break ;
                default:
					std::cerr << "Unhandled item subtype " << (uint32_t) ni->PacketSubType() << " in RsGxsNetService: " << std::endl; break;
			}
			delete item ;
		}
		else
		{
			std::cerr << "Not a RsNxsItem, deleting!" << std::endl;
			delete(item);
		}
	}
}


bool RsGxsNetService::handleTransaction(RsNxsItem* item)
{

	/*!
	 * This attempts to handle a transaction
	 * It first checks if this transaction id already exists
	 * If it does then check this not a initiating transactions
	 */

	RS_STACK_MUTEX(mNxsMutex) ;

	const RsPeerId& peer = item->PeerId();

	RsNxsTransac* transItem = dynamic_cast<RsNxsTransac*>(item);

	// if this is a RsNxsTransac item process
	if(transItem)
		return locked_processTransac(transItem);


	// then this must be transaction content to be consumed
	// first check peer exist for transaction
	bool peerTransExists = mTransactions.find(peer) != mTransactions.end();

	// then check transaction exists

	bool transExists = false;
	NxsTransaction* tr = NULL;
	uint32_t transN = item->transactionNumber;

	if(peerTransExists)
	{
		TransactionIdMap& transMap = mTransactions[peer];

		transExists = transMap.find(transN) != transMap.end();

		if(transExists)
		{

#ifdef NXS_NET_DEBUG
			std::cerr << "handleTransaction() " << std::endl;
			std::cerr << "Consuming Transaction content, transN: " << item->transactionNumber << std::endl;
			std::cerr << "Consuming Transaction content, from Peer: " << item->PeerId() << std::endl;
#endif

			tr = transMap[transN];
			tr->mItems.push_back(item);

			return true;
		}
	}

	return false;
}

bool RsGxsNetService::locked_processTransac(RsNxsTransac* item)
{

	/*!
	 * To process the transaction item
	 * It can either be initiating a transaction
	 * or ending one that already exists
	 *
	 * For initiating an incoming transaction the peer
	 * and transaction item need not exists
	 * as the peer will be added and transaction number
	 * added thereafter
	 *
	 * For commencing/starting an outgoing transaction
	 * the transaction must exist already
	 *
	 * For ending a transaction the
	 */

	RsPeerId peer;

	// for outgoing transaction use own id
	if(item->transactFlag & (RsNxsTransac::FLAG_BEGIN_P2 | RsNxsTransac::FLAG_END_SUCCESS))
		peer = mOwnId;
	else
		peer = item->PeerId();

	uint32_t transN = item->transactionNumber;
	item->timestamp = time(NULL); // register time received
	NxsTransaction* tr = NULL;

#ifdef NXS_NET_DEBUG
	std::cerr << "locked_processTransac() " << std::endl;
	std::cerr << "locked_processTransac(), Received transaction item: " << transN << std::endl;
	std::cerr << "locked_processTransac(), With peer: " << item->PeerId() << std::endl;
	std::cerr << "locked_processTransac(), trans type: " << item->transactFlag << std::endl;
#endif

	bool peerTrExists = mTransactions.find(peer) != mTransactions.end();
	bool transExists = false;

	if(peerTrExists){

		TransactionIdMap& transMap = mTransactions[peer];
		// record whether transaction exists already
		transExists = transMap.find(transN) != transMap.end();

	}

	// initiating an incoming transaction
	if(item->transactFlag & RsNxsTransac::FLAG_BEGIN_P1){

		if(transExists)
			return false; // should not happen!

		// create a transaction if the peer does not exist
		if(!peerTrExists){
			mTransactions[peer] = TransactionIdMap();
		}

		TransactionIdMap& transMap = mTransactions[peer];


		// create new transaction
		tr = new NxsTransaction();
		transMap[transN] = tr;
		tr->mTransaction = item;
        tr->mTimeOut = item->timestamp + mTransactionTimeOut;

		// note state as receiving, commencement item
		// is sent on next run() loop
		tr->mFlag = NxsTransaction::FLAG_STATE_STARTING;
        return true;
		// commencement item for outgoing transaction
	}else if(item->transactFlag & RsNxsTransac::FLAG_BEGIN_P2){

		// transaction must exist
		if(!peerTrExists || !transExists)
			return false;


		// alter state so transaction content is sent on
		// next run() loop
		TransactionIdMap& transMap = mTransactions[mOwnId];
		NxsTransaction* tr = transMap[transN];
		tr->mFlag = NxsTransaction::FLAG_STATE_SENDING;
        delete item;
        return true;
		// end transac item for outgoing transaction
	}else if(item->transactFlag & RsNxsTransac::FLAG_END_SUCCESS){

		// transaction does not exist
		if(!peerTrExists || !transExists){
			return false;
		}

		// alter state so that transaction is removed
		// on next run() loop
		TransactionIdMap& transMap = mTransactions[mOwnId];
		NxsTransaction* tr = transMap[transN];
        tr->mFlag = NxsTransaction::FLAG_STATE_COMPLETED;
        delete item;
        return true;
    }
    else
        return false;
}

void RsGxsNetService::run(){
    double timeDelta = 0.5;
    int updateCounter = 0;

    while(isRunning()){
        //Start waiting as nothing to do in runup
        usleep((int) (timeDelta * 1000 * 1000)); // timeDelta sec

        if(updateCounter >= 20) {
        	updateServerSyncTS();
        	updateCounter = 0;
        } else {//if(updateCounter >= 20)
        	updateCounter++;
        }//else (updateCounter >= 20)

        // process active transactions
        processTransactions();

        // process completed transactions
        processCompletedTransactions();

        // vetting of id and circle info
        runVetting();

        processExplicitGroupRequests();

    }//while(isRunning())
}

void RsGxsNetService::updateServerSyncTS()
{
	RS_STACK_MUTEX(mNxsMutex) ;

	std::map<RsGxsGroupId, RsGxsGrpMetaData*> gxsMap;

	// retrieve all grps and update TS
	mDataStore->retrieveGxsGrpMetaData(gxsMap);
	std::map<RsGxsGroupId, RsGxsGrpMetaData*>::iterator mit = gxsMap.begin();

	// as a grp list server also note this is the latest item you have
	if(mGrpServerUpdateItem == NULL)
	{
		mGrpServerUpdateItem = new RsGxsServerGrpUpdateItem(mServType);
	}

	bool change = false;

	for(; mit != gxsMap.end(); ++mit)
	{
		const RsGxsGroupId& grpId = mit->first;
		RsGxsGrpMetaData* grpMeta = mit->second;
		ServerMsgMap::iterator mapIT = mServerMsgUpdateMap.find(grpId);
		RsGxsServerMsgUpdateItem* msui = NULL;

		if(mapIT == mServerMsgUpdateMap.end())
		{
			msui = new RsGxsServerMsgUpdateItem(mServType);
			msui->grpId = grpMeta->mGroupId;
			mServerMsgUpdateMap.insert(std::make_pair(msui->grpId, msui));
		}else
		{
			msui = mapIT->second;
		}

		if(grpMeta->mLastPost > msui->msgUpdateTS )
		{
			change = true;
			msui->msgUpdateTS = grpMeta->mLastPost;
		}

		// this might be very inefficient with time
		if(grpMeta->mRecvTS > mGrpServerUpdateItem->grpUpdateTS)
		{
			mGrpServerUpdateItem->grpUpdateTS = grpMeta->mRecvTS;
			change = true;
		}
	}

	// actual change in config settings, then save configuration
	if(change)
		IndicateConfigChanged();

	freeAndClearContainerResource<std::map<RsGxsGroupId, RsGxsGrpMetaData*>,
		RsGxsGrpMetaData*>(gxsMap);

}
bool RsGxsNetService::locked_checkTransacTimedOut(NxsTransaction* tr)
{
   return tr->mTimeOut < ((uint32_t) time(NULL));
}

void RsGxsNetService::processTransactions(){

	RS_STACK_MUTEX(mNxsMutex) ;

	TransactionsPeerMap::iterator mit = mTransactions.begin();

	for(; mit != mTransactions.end(); ++mit){

		TransactionIdMap& transMap = mit->second;
		TransactionIdMap::iterator mmit = transMap.begin(),

		mmit_end = transMap.end();

		// transaction to be removed
		std::list<uint32_t> toRemove;

		/*!
		 * Transactions owned by peer
		 */
		if(mit->first == mOwnId){

			for(; mmit != mmit_end; ++mmit){

				NxsTransaction* tr = mmit->second;
				uint16_t flag = tr->mFlag;
				std::list<RsNxsItem*>::iterator lit, lit_end;
				uint32_t transN = tr->mTransaction->transactionNumber;

				// first check transaction has not expired
				if(locked_checkTransacTimedOut(tr))
				{
					std::cerr << std::dec ;
					int total_transaction_time = (int)time(NULL) - (tr->mTimeOut - mTransactionTimeOut) ;
					std::cerr << "Outgoing Transaction has failed, tranN: " << transN << ", Peer: " << mit->first ;
					std::cerr << ", age: " << total_transaction_time << ", nItems=" << tr->mTransaction->nItems << ". tr->mTimeOut = " << tr->mTimeOut << ", now = " << (uint32_t) time(NULL) << std::endl;

					tr->mFlag = NxsTransaction::FLAG_STATE_FAILED;
					toRemove.push_back(transN);
					mComplTransactions.push_back(tr);
					continue;
				}

				// send items requested
				if(flag & NxsTransaction::FLAG_STATE_SENDING){

#ifdef NXS_NET_DEBUG
					std::cerr << "processTransactions() " << std::endl;
					std::cerr << "Sending Transaction content, transN: " << transN << std::endl;
					std::cerr << "with peer: " << tr->mTransaction->PeerId();
#endif
					lit = tr->mItems.begin();
					lit_end = tr->mItems.end();

					for(; lit != lit_end; ++lit){
						sendItem(*lit);
					}

					tr->mItems.clear(); // clear so they don't get deleted in trans cleaning
					tr->mFlag = NxsTransaction::FLAG_STATE_WAITING_CONFIRM;

				}else if(flag & NxsTransaction::FLAG_STATE_WAITING_CONFIRM){
					continue;

				}else if(flag & NxsTransaction::FLAG_STATE_COMPLETED){

#ifdef NXS_NET_DEBUG
                    int total_transaction_time = (int)time(NULL) - (tr->mTimeOut - mTransactionTimeOut) ;

                    std::cerr << "RsGxsNetService::processTransactions() outgoing completed " << tr->mTransaction->nItems << " items transaction in " << total_transaction_time << " seconds." << std::endl;
#endif
                    // move to completed transactions
					toRemove.push_back(transN);
					mComplTransactions.push_back(tr);
				}else{

#ifdef NXS_NET_DEBUG
					std::cerr << "processTransactions() " << std::endl;
					std::cerr << "processTransactions(), Unknown flag for active transaction, transN: " << transN
							  << std::endl;
					std::cerr << "processTransactions(), Unknown flag, Peer: " << mit->first;
#endif

					toRemove.push_back(transN);
					tr->mFlag = NxsTransaction::FLAG_STATE_FAILED;
					mComplTransactions.push_back(tr);
				}
			}

		}else{

			/*!
			 * Essentially these are incoming transactions
			 * Several states are dealth with
			 * Receiving: waiting to receive items from peer's transaction
			 * and checking if all have been received
			 * Completed: remove transaction from active and tell peer
			 * involved in transaction
			 * Starting: this is a new transaction and need to teell peer
			 * involved in transaction
			 */

			for(; mmit != mmit_end; ++mmit){

				NxsTransaction* tr = mmit->second;
				uint16_t flag = tr->mFlag;
				uint32_t transN = tr->mTransaction->transactionNumber;

				// first check transaction has not expired
				if(locked_checkTransacTimedOut(tr))
				{
					std::cerr << std::dec ;
					int total_transaction_time = (int)time(NULL) - (tr->mTimeOut - mTransactionTimeOut) ;
					std::cerr << "Incoming Transaction has failed, tranN: " << transN << ", Peer: " << mit->first ;
					std::cerr << ", age: " << total_transaction_time << ", nItems=" << tr->mTransaction->nItems << ". tr->mTimeOut = " << tr->mTimeOut << ", now = " << (uint32_t) time(NULL) << std::endl;

					tr->mFlag = NxsTransaction::FLAG_STATE_FAILED;
					toRemove.push_back(transN);
					mComplTransactions.push_back(tr);
					continue;
				}

				if(flag & NxsTransaction::FLAG_STATE_RECEIVING){

					// if the number it item received equal that indicated
					// then transaction is marked as completed
					// to be moved to complete transations
					// check if done
                    if(tr->mItems.size() == tr->mTransaction->nItems)
						tr->mFlag = NxsTransaction::FLAG_STATE_COMPLETED;

				}else if(flag & NxsTransaction::FLAG_STATE_COMPLETED)
				{

					// send completion msg
					RsNxsTransac* trans = new RsNxsTransac(mServType);
					trans->clear();
					trans->transactFlag = RsNxsTransac::FLAG_END_SUCCESS;
					trans->transactionNumber = transN;
					trans->PeerId(tr->mTransaction->PeerId());
					sendItem(trans);

					// move to completed transactions
					mComplTransactions.push_back(tr);
#ifdef NXS_NET_DEBUG
					int total_transaction_time = (int)time(NULL) - (tr->mTimeOut - mTransactionTimeOut) ;
					std::cerr << "RsGxsNetService::processTransactions() incoming completed " << tr->mTransaction->nItems << " items transaction in " << total_transaction_time << " seconds." << std::endl;
#endif

					// transaction processing done
					// for this id, add to removal list
					toRemove.push_back(mmit->first);
				}else if(flag & NxsTransaction::FLAG_STATE_STARTING){

					// send item to tell peer your are ready to start
					RsNxsTransac* trans = new RsNxsTransac(mServType);
					trans->clear();
					trans->transactFlag = RsNxsTransac::FLAG_BEGIN_P2 |
							(tr->mTransaction->transactFlag & RsNxsTransac::FLAG_TYPE_MASK);
					trans->transactionNumber = transN;
					trans->PeerId(tr->mTransaction->PeerId());
					sendItem(trans);
					tr->mFlag = NxsTransaction::FLAG_STATE_RECEIVING;

				}
				else{

					std::cerr << "processTransactions() " << std::endl;
					std::cerr << "processTransactions(), Unknown flag for active transaction, transN: " << transN
							  << std::endl;
					std::cerr << "processTransactions(), Unknown flag, Peer: " << mit->first;
					toRemove.push_back(mmit->first);
					mComplTransactions.push_back(tr);
					tr->mFlag = NxsTransaction::FLAG_STATE_FAILED; // flag as a failed transaction
				}
			}
		}

		std::list<uint32_t>::iterator lit = toRemove.begin();

		for(; lit != toRemove.end(); ++lit)
		{
			transMap.erase(*lit);
		}

	}
}

int RsGxsNetService::getGroupPopularity(const RsGxsGroupId& gid)
{
	RS_STACK_MUTEX(mNxsMutex) ;

    std::map<RsGxsGroupId,std::set<RsPeerId> >::const_iterator it = mGroupSuppliers.find(gid) ;

    if(it == mGroupSuppliers.end())
        return 0 ;
    else
        return it->second.size();
}

void RsGxsNetService::processCompletedTransactions()
{
	RS_STACK_MUTEX(mNxsMutex) ;
	/*!
	 * Depending on transaction we may have to respond to peer
	 * responsible for transaction
	 */
	std::list<NxsTransaction*>::iterator lit = mComplTransactions.begin();

	while(mComplTransactions.size()>0)
	{

		NxsTransaction* tr = mComplTransactions.front();

		bool outgoing = tr->mTransaction->PeerId() == mOwnId;

		if(outgoing){
			locked_processCompletedOutgoingTrans(tr);
		}else{
			locked_processCompletedIncomingTrans(tr);
		}


		delete tr;
		mComplTransactions.pop_front();
	}
}

void RsGxsNetService::locked_processCompletedIncomingTrans(NxsTransaction* tr)
{

	uint16_t flag = tr->mTransaction->transactFlag;

	if(tr->mFlag & NxsTransaction::FLAG_STATE_COMPLETED){
				// for a completed list response transaction
				// one needs generate requests from this
				if(flag & RsNxsTransac::FLAG_TYPE_MSG_LIST_RESP)
				{
					// generate request based on a peers response
					locked_genReqMsgTransaction(tr);

				}else if(flag & RsNxsTransac::FLAG_TYPE_GRP_LIST_RESP)
				{
					locked_genReqGrpTransaction(tr);
				}
				// you've finished receiving request information now gen
				else if(flag & RsNxsTransac::FLAG_TYPE_MSG_LIST_REQ)
				{
					locked_genSendMsgsTransaction(tr);
				}
				else if(flag & RsNxsTransac::FLAG_TYPE_GRP_LIST_REQ)
				{
					locked_genSendGrpsTransaction(tr);
				}
				else if(flag & RsNxsTransac::FLAG_TYPE_GRPS)
				{

					std::list<RsNxsItem*>::iterator lit = tr->mItems.begin();
					std::vector<RsNxsGrp*> grps;

					while(tr->mItems.size() != 0)
					{
						RsNxsGrp* grp = dynamic_cast<RsNxsGrp*>(tr->mItems.front());

						if(grp)
						{
							tr->mItems.pop_front();
                            grps.push_back(grp);

						}
						else
						{
		#ifdef NXS_NET_DEBUG
							std::cerr << "RsGxsNetService::processCompletedTransactions(): item did not caste to grp"
									  << std::endl;
		#endif
						}
					}

					// notify listener of grps
					mObserver->notifyNewGroups(grps);

					// now note this as the latest you've received from this peer
					RsPeerId peerFrom = tr->mTransaction->PeerId();
					uint32_t updateTS = tr->mTransaction->updateTS;

					ClientGrpMap::iterator it = mClientGrpUpdateMap.find(peerFrom);

					RsGxsGrpUpdateItem* item = NULL;

					if(it != mClientGrpUpdateMap.end())
					{
						item = it->second;
					}else
					{
						item = new RsGxsGrpUpdateItem(mServType);
						mClientGrpUpdateMap.insert(
								std::make_pair(peerFrom, item));
					}

					item->grpUpdateTS = updateTS;
					item->peerId = peerFrom;

					IndicateConfigChanged();


				}else if(flag & RsNxsTransac::FLAG_TYPE_MSGS)
				{

					std::vector<RsNxsMsg*> msgs;

                                        RsGxsGroupId grpId;
					while(tr->mItems.size() > 0)
					{
						RsNxsMsg* msg = dynamic_cast<RsNxsMsg*>(tr->mItems.front());
						if(msg)
						{
                                                    if(grpId.isNull())
                                                        grpId = msg->grpId;

                                                    tr->mItems.pop_front();
                                                    msgs.push_back(msg);
						}
						else
						{
		#ifdef NXS_NET_DEBUG
                                                    std::cerr << "RsGxsNetService::processCompletedTransactions(): item did not caste to msg"
                                                                      << std::endl;
		#endif
						}
					}

#ifdef NSXS_FRAG
					std::map<RsGxsGroupId, MsgFragments > collatedMsgs;
					collateMsgFragments(msgs, collatedMsgs);

					msgs.clear();

					std::map<RsGxsGroupId, MsgFragments >::iterator mit = collatedMsgs.begin();
					for(; mit != collatedMsgs.end(); ++mit)
					{
						MsgFragments& f = mit->second;
						RsNxsMsg* msg = deFragmentMsg(f);

						if(msg)
							msgs.push_back(msg);
					}
#endif
					// notify listener of msgs
					mObserver->notifyNewMessages(msgs);

                                        // now note that this is the latest you've received from this peer
                                        // for the grp id
                                        locked_doMsgUpdateWork(tr->mTransaction, grpId);

				}
			}else if(tr->mFlag == NxsTransaction::FLAG_STATE_FAILED){
				// don't do anything transaction will simply be cleaned
			}
	return;
}

void RsGxsNetService::locked_doMsgUpdateWork(const RsNxsTransac *nxsTrans, const RsGxsGroupId &grpId)
{

    // firts check if peer exists
    const RsPeerId& peerFrom = nxsTrans->PeerId();

    ClientMsgMap::iterator it = mClientMsgUpdateMap.find(peerFrom);

    RsGxsMsgUpdateItem* mui = NULL;

    // now update the peer's entry for this grp id
    if(it != mClientMsgUpdateMap.end())
    {
        mui = it->second;
    }
    else
    {
        mui = new RsGxsMsgUpdateItem(mServType);
        mClientMsgUpdateMap.insert(std::make_pair(peerFrom, mui));
    }

    mui->msgUpdateTS[grpId] = nxsTrans->updateTS;
    mui->peerId = peerFrom;

    IndicateConfigChanged();
}

void RsGxsNetService::locked_processCompletedOutgoingTrans(NxsTransaction* tr)
{
	uint16_t flag = tr->mTransaction->transactFlag;

	if(tr->mFlag & NxsTransaction::FLAG_STATE_COMPLETED){
				// for a completed list response transaction
				// one needs generate requests from this
				if(flag & RsNxsTransac::FLAG_TYPE_MSG_LIST_RESP)
				{
#ifdef NXS_NET_DEBUG
					std::cerr << "processCompletedOutgoingTrans()" << std::endl;
					std::cerr << "complete Sending Msg List Response, transN: " <<
							  tr->mTransaction->transactionNumber << std::endl;
#endif
				}else if(flag & RsNxsTransac::FLAG_TYPE_GRP_LIST_RESP)
				{
#ifdef NXS_NET_DEBUG
					std::cerr << "processCompletedOutgoingTrans()" << std::endl;
					std::cerr << "complete Sending Grp Response, transN: " <<
							  tr->mTransaction->transactionNumber << std::endl;
#endif
				}
				// you've finished sending a request so don't do anything
				else if( (flag & RsNxsTransac::FLAG_TYPE_MSG_LIST_REQ) ||
						(flag & RsNxsTransac::FLAG_TYPE_GRP_LIST_REQ) )
				{
#ifdef NXS_NET_DEBUG
					std::cerr << "processCompletedOutgoingTrans()" << std::endl;
					std::cerr << "complete Sending Msg/Grp Request, transN: " <<
							  tr->mTransaction->transactionNumber << std::endl;
#endif

				}else if(flag & RsNxsTransac::FLAG_TYPE_GRPS)
				{

#ifdef NXS_NET_DEBUG
					std::cerr << "processCompletedOutgoingTrans()" << std::endl;
					std::cerr << "complete Sending Grp Data, transN: " <<
							  tr->mTransaction->transactionNumber << std::endl;
#endif

				}else if(flag & RsNxsTransac::FLAG_TYPE_MSGS)
				{
#ifdef NXS_NET_DEBUG
					std::cerr << "processCompletedOutgoingTrans()" << std::endl;
					std::cerr << "complete Sending Msg Data, transN: " <<
							  tr->mTransaction->transactionNumber << std::endl;
#endif
				}
			}else if(tr->mFlag == NxsTransaction::FLAG_STATE_FAILED){
#ifdef NXS_NET_DEBUG
				std::cerr << "processCompletedOutgoingTrans()" << std::endl;
						  std::cerr << "Failed transaction! transN: " <<
						  tr->mTransaction->transactionNumber << std::endl;
#endif
			}else{

#ifdef NXS_NET_DEBUG
					std::cerr << "processCompletedOutgoingTrans()" << std::endl;
					std::cerr << "Serious error unrecognised trans Flag! transN: " <<
							  tr->mTransaction->transactionNumber << std::endl;
#endif
			}
}


void RsGxsNetService::locked_pushMsgTransactionFromList(
		std::list<RsNxsItem*>& reqList, const RsPeerId& peerId, const uint32_t& transN)
{
	RsNxsTransac* transac = new RsNxsTransac(mServType);
	transac->transactFlag = RsNxsTransac::FLAG_TYPE_MSG_LIST_REQ
			| RsNxsTransac::FLAG_BEGIN_P1;
	transac->timestamp = 0;
	transac->nItems = reqList.size();
	transac->PeerId(peerId);
	transac->transactionNumber = transN;
	NxsTransaction* newTrans = new NxsTransaction();
	newTrans->mItems = reqList;
	newTrans->mFlag = NxsTransaction::FLAG_STATE_WAITING_CONFIRM;
    newTrans->mTimeOut = time(NULL) + mTransactionTimeOut;
	// create transaction copy with your id to indicate
	// its an outgoing transaction
        newTrans->mTransaction = new RsNxsTransac(*transac);
	newTrans->mTransaction->PeerId(mOwnId);
	sendItem(transac);
	{
		if (!locked_addTransaction(newTrans))
			delete newTrans;
	}
}

void RsGxsNetService::locked_genReqMsgTransaction(NxsTransaction* tr)
{

	// to create a transaction you need to know who you are transacting with
	// then what msgs to request
	// then add an active Transaction for request

	std::list<RsNxsSyncMsgItem*> msgItemL;

	std::list<RsNxsItem*>::iterator lit = tr->mItems.begin();

	// first get item list sent from transaction
	for(; lit != tr->mItems.end(); ++lit)
	{
		RsNxsSyncMsgItem* item = dynamic_cast<RsNxsSyncMsgItem*>(*lit);
		if(item)
		{
			msgItemL.push_back(item);
		}else
		{
#ifdef NXS_NET_DEBUG
		std::cerr << "RsGxsNetService::genReqMsgTransaction(): item failed cast to RsNxsSyncMsgItem* "
				  << std::endl;
#endif
		}
	}

	if(msgItemL.empty())
		return;

	// get grp id for this transaction
	RsNxsSyncMsgItem* item = msgItemL.front();
	const RsGxsGroupId& grpId = item->grpId;

	std::map<RsGxsGroupId, RsGxsGrpMetaData*> grpMetaMap;
	grpMetaMap[grpId] = NULL;
	mDataStore->retrieveGxsGrpMetaData(grpMetaMap);
	RsGxsGrpMetaData* grpMeta = grpMetaMap[grpId];

	int cutoff = 0;
	if(grpMeta != NULL)
		cutoff = grpMeta->mReputationCutOff;
//
//	// you want to find out if you can receive it
//	// number polls essentially represent multiple
//	// of sleep interval
//	if(grpMeta)
//	{
//		// always can receive, only provides weak guaranttee this peer is part of the group
//		bool can = true;//locked_canReceive(grpMeta, tr->mTransaction->PeerId());
//
//		delete grpMeta;
//
//		if(!can)
//			return;
//
//	}else
//	{
//		return;
//	}


	GxsMsgReq reqIds;
	reqIds[grpId] = std::vector<RsGxsMessageId>();
	GxsMsgMetaResult result;
	mDataStore->retrieveGxsMsgMetaData(reqIds, result);
	std::vector<RsGxsMsgMetaData*> &msgMetaV = result[grpId];

	std::vector<RsGxsMsgMetaData*>::const_iterator vit = msgMetaV.begin();
	std::set<RsGxsMessageId> msgIdSet;

	// put ids in set for each searching
	for(; vit != msgMetaV.end(); ++vit)
	{
		msgIdSet.insert((*vit)->mMsgId);
		delete(*vit);
	}
        msgMetaV.clear();

	// get unique id for this transaction
	uint32_t transN = locked_getTransactionId();

	// add msgs that you don't have to request list
	std::list<RsNxsSyncMsgItem*>::iterator llit = msgItemL.begin();
	std::list<RsNxsItem*> reqList;

	const RsPeerId peerFrom = tr->mTransaction->PeerId();

	MsgAuthorV toVet;

	std::list<RsPeerId> peers;
	peers.push_back(tr->mTransaction->PeerId());

	for(; llit != msgItemL.end(); ++llit)
	{
		RsNxsSyncMsgItem*& syncItem = *llit;
		const RsGxsMessageId& msgId = syncItem->msgId;

		if(msgIdSet.find(msgId) == msgIdSet.end()){

			// if reputation is in reputations cache then proceed
			// or if there isn't an author (note as author requirement is
			// enforced at service level, if no author is needed then reputation
			// filtering is optional)
			bool noAuthor = syncItem->authorId.isNull();

			// grp meta must be present if author present
			if(!noAuthor && grpMeta == NULL)
				continue;

			if(mReputations->haveReputation(syncItem->authorId) || noAuthor)
			{
				GixsReputation rep;

				if(!noAuthor)
					mReputations->getReputation(syncItem->authorId, rep);

				// if author is required for this message, it will simply get dropped
				// at genexchange side of things
				if(rep.score > grpMeta->mReputationCutOff || noAuthor)
				{
					RsNxsSyncMsgItem* msgItem = new RsNxsSyncMsgItem(mServType);
					msgItem->grpId = grpId;
					msgItem->msgId = msgId;
					msgItem->flag = RsNxsSyncMsgItem::FLAG_REQUEST;
					msgItem->transactionNumber = transN;
					msgItem->PeerId(peerFrom);
					reqList.push_back(msgItem);
				}
			}
			else
			{
				// preload for speed
				mReputations->loadReputation(syncItem->authorId, peers);
				MsgAuthEntry entry;
				entry.mAuthorId = syncItem->authorId;
				entry.mGrpId = syncItem->grpId;
				entry.mMsgId = syncItem->msgId;
				toVet.push_back(entry);
			}
		}
	}

	if(!toVet.empty())
	{
		MsgRespPending* mrp = new MsgRespPending(mReputations, tr->mTransaction->PeerId(), toVet, cutoff);
		mPendingResp.push_back(mrp);
	}

	if(!reqList.empty())
	{
		locked_pushMsgTransactionFromList(reqList, tr->mTransaction->PeerId(), transN);
	}
}

void RsGxsNetService::locked_pushGrpTransactionFromList(
		std::list<RsNxsItem*>& reqList, const RsPeerId& peerId, const uint32_t& transN)
{
	RsNxsTransac* transac = new RsNxsTransac(mServType);
	transac->transactFlag = RsNxsTransac::FLAG_TYPE_GRP_LIST_REQ
			| RsNxsTransac::FLAG_BEGIN_P1;
	transac->timestamp = 0;
	transac->nItems = reqList.size();
	transac->PeerId(peerId);
	transac->transactionNumber = transN;
	NxsTransaction* newTrans = new NxsTransaction();
	newTrans->mItems = reqList;
	newTrans->mFlag = NxsTransaction::FLAG_STATE_WAITING_CONFIRM;
    newTrans->mTimeOut = time(NULL) + mTransactionTimeOut;
	newTrans->mTransaction = new RsNxsTransac(*transac);
	newTrans->mTransaction->PeerId(mOwnId);
	sendItem(transac);
	if (!locked_addTransaction(newTrans))
		delete newTrans;
}
void RsGxsNetService::addGroupItemToList(NxsTransaction*& tr,
		const RsGxsGroupId& grpId, uint32_t& transN,
		std::list<RsNxsItem*>& reqList)
{
#ifdef NXS_NET_DEBUG
	std::cerr << "RsGxsNetService::addGroupItemToList() Added GroupID: << grpId";
	std::cerr << std::endl;
#endif

	RsNxsSyncGrpItem* grpItem = new RsNxsSyncGrpItem(mServType);
	grpItem->PeerId(tr->mTransaction->PeerId());
	grpItem->grpId = grpId;
	grpItem->flag = RsNxsSyncMsgItem::FLAG_REQUEST;
	grpItem->transactionNumber = transN;
	reqList.push_back(grpItem);
}

void RsGxsNetService::locked_genReqGrpTransaction(NxsTransaction* tr)
{

	// to create a transaction you need to know who you are transacting with
	// then what grps to request
	// then add an active Transaction for request

	std::list<RsNxsSyncGrpItem*> grpItemL;

	std::list<RsNxsItem*>::iterator lit = tr->mItems.begin();

	for(; lit != tr->mItems.end(); ++lit)
	{
		RsNxsSyncGrpItem* item = dynamic_cast<RsNxsSyncGrpItem*>(*lit);
		if(item)
		{
			grpItemL.push_back(item);
		}else
		{
#ifdef NXS_NET_DEBUG
			std::cerr << "RsGxsNetService::genReqGrpTransaction(): item failed to caste to RsNxsSyncMsgItem* "
					  << std::endl;
#endif
		}
	}

    std::map<RsGxsGroupId, RsGxsGrpMetaData*> grpMetaMap;
    std::map<RsGxsGroupId, RsGxsGrpMetaData*>::const_iterator metaIter;
	mDataStore->retrieveGxsGrpMetaData(grpMetaMap);

	// now do compare and add loop
	std::list<RsNxsSyncGrpItem*>::iterator llit = grpItemL.begin();
	std::list<RsNxsItem*> reqList;

	uint32_t transN = locked_getTransactionId();

	GrpAuthorV toVet;
	std::list<RsPeerId> peers;
	peers.push_back(tr->mTransaction->PeerId());

	for(; llit != grpItemL.end(); ++llit)
	{
		RsNxsSyncGrpItem*& grpSyncItem = *llit;
		const RsGxsGroupId& grpId = grpSyncItem->grpId;
		metaIter = grpMetaMap.find(grpId);
		bool haveItem = false;
		bool latestVersion = false;
		if (metaIter != grpMetaMap.end())
		{
			haveItem = true;
			latestVersion = grpSyncItem->publishTs > metaIter->second->mPublishTs;
		}

		if(!haveItem || (haveItem && latestVersion) ){

			// determine if you need to check reputation
			bool checkRep = !grpSyncItem->authorId.isNull();

			// check if you have reputation, if you don't then
			// place in holding pen
			if(checkRep)
			{
				if(mReputations->haveReputation(grpSyncItem->authorId))
				{
					GixsReputation rep;
					mReputations->getReputation(grpSyncItem->authorId, rep);

					if(rep.score > GIXS_CUT_OFF)
					{
						addGroupItemToList(tr, grpId, transN, reqList);
					}
				}
				else
				{
					// preload reputation for later
					mReputations->loadReputation(grpSyncItem->authorId, peers);
					GrpAuthEntry entry;
					entry.mAuthorId = grpSyncItem->authorId;
					entry.mGrpId = grpSyncItem->grpId;
					toVet.push_back(entry);
				}
			}
			else
			{
				addGroupItemToList(tr, grpId, transN, reqList);
			}
		}
	}

	if(!toVet.empty())
	{
		RsPeerId peerId = tr->mTransaction->PeerId();
		GrpRespPending* grp = new GrpRespPending(mReputations, peerId, toVet);
		mPendingResp.push_back(grp);
	}


	if(!reqList.empty())
	{
		locked_pushGrpTransactionFromList(reqList, tr->mTransaction->PeerId(), transN);

	}

	// clean up meta data
    std::map<RsGxsGroupId, RsGxsGrpMetaData*>::iterator mit = grpMetaMap.begin();

	for(; mit != grpMetaMap.end(); ++mit)
		delete mit->second;
}

void RsGxsNetService::locked_genSendGrpsTransaction(NxsTransaction* tr)
{

#ifdef NXS_NET_DEBUG
	std::cerr << "locked_genSendGrpsTransaction()" << std::endl;
	std::cerr << "Generating Grp data send fron TransN: " << tr->mTransaction->transactionNumber
	          << std::endl;
#endif

	// go groups requested in transaction tr

	std::list<RsNxsItem*>::iterator lit = tr->mItems.begin();

	std::map<RsGxsGroupId, RsNxsGrp*> grps;

	for(;lit != tr->mItems.end(); ++lit)
	{
		RsNxsSyncGrpItem* item = dynamic_cast<RsNxsSyncGrpItem*>(*lit);
		if (item)
		{
			grps[item->grpId] = NULL;
		}else
		{
#ifdef NXS_NET_DEBUG
			std::cerr << "RsGxsNetService::locked_genSendGrpsTransaction(): item failed to caste to RsNxsSyncGrpItem* "
					  << std::endl;
#endif
		}
	}

	if(!grps.empty())
	{
		mDataStore->retrieveNxsGrps(grps, false, false);
	}
	else{
		return;
	}

	NxsTransaction* newTr = new NxsTransaction();
	newTr->mFlag = NxsTransaction::FLAG_STATE_WAITING_CONFIRM;

	uint32_t transN = locked_getTransactionId();

	// store grp items to send in transaction
	std::map<RsGxsGroupId, RsNxsGrp*>::iterator mit = grps.begin();
	RsPeerId peerId = tr->mTransaction->PeerId();
	for(;mit != grps.end(); ++mit)
	{
		mit->second->PeerId(peerId); // set so it gets sent to right peer
		mit->second->transactionNumber = transN;
		newTr->mItems.push_back(mit->second);
	}

	if(newTr->mItems.empty()){
		delete newTr;
		return;
	}

	uint32_t updateTS = 0;
	if(mGrpServerUpdateItem)
		updateTS = mGrpServerUpdateItem->grpUpdateTS;

	RsNxsTransac* ntr = new RsNxsTransac(mServType);
	ntr->transactionNumber = transN;
	ntr->transactFlag = RsNxsTransac::FLAG_BEGIN_P1 |
			RsNxsTransac::FLAG_TYPE_GRPS;
        ntr->updateTS = updateTS;
	ntr->nItems = grps.size();
	ntr->PeerId(tr->mTransaction->PeerId());

	newTr->mTransaction = new RsNxsTransac(*ntr);
	newTr->mTransaction->PeerId(mOwnId);
	newTr->mTimeOut = time(NULL) + mTransactionTimeOut;

	ntr->PeerId(tr->mTransaction->PeerId());
	sendItem(ntr);

	locked_addTransaction(newTr);

	return;
}

void RsGxsNetService::runVetting()
{
	RS_STACK_MUTEX(mNxsMutex) ;

	std::vector<AuthorPending*>::iterator vit = mPendingResp.begin();

	for(; vit != mPendingResp.end(); )
	{
		AuthorPending* ap = *vit;

		if(ap->accepted() || ap->expired())
		{
			// add to transactions
			if(AuthorPending::MSG_PEND == ap->getType())
			{
				MsgRespPending* mrp = static_cast<MsgRespPending*>(ap);
				locked_createTransactionFromPending(mrp);
			}
			else if(AuthorPending::GRP_PEND == ap->getType())
			{
				GrpRespPending* grp = static_cast<GrpRespPending*>(ap);
				locked_createTransactionFromPending(grp);
			}else
			{
#ifdef NXS_NET_DEBUG
				std::cerr << "RsGxsNetService::runVetting(): Unknown pending type! Type: " << ap->getType()
						  << std::endl;
#endif
			}

			delete ap;
			vit = mPendingResp.erase(vit);
		}
		else
		{
			++vit;
		}

	}


	// now lets do circle vetting
	std::vector<GrpCircleVetting*>::iterator vit2 = mPendingCircleVets.begin();
	for(; vit2 != mPendingCircleVets.end(); )
	{
		GrpCircleVetting*& gcv = *vit2;
		if(gcv->cleared() || gcv->expired())
		{
			if(gcv->getType() == GrpCircleVetting::GRP_ID_PEND)
			{
				GrpCircleIdRequestVetting* gcirv =
						static_cast<GrpCircleIdRequestVetting*>(gcv);

				locked_createTransactionFromPending(gcirv);
			}
			else if(gcv->getType() == GrpCircleVetting::MSG_ID_SEND_PEND)
			{
				MsgCircleIdsRequestVetting* mcirv =
						static_cast<MsgCircleIdsRequestVetting*>(gcv);

				if(mcirv->cleared())
					locked_createTransactionFromPending(mcirv);
			}
			else
			{
#ifdef NXS_NET_DEBUG
				std::cerr << "RsGxsNetService::runVetting(): Unknown Circle pending type! Type: " << gcv->getType()
						  << std::endl;
#endif
			}

			delete gcv;
			vit2 = mPendingCircleVets.erase(vit2);
		}
		else
		{
			++vit2;
		}
	}
}

void RsGxsNetService::locked_genSendMsgsTransaction(NxsTransaction* tr)
{

#ifdef NXS_NET_DEBUG
	std::cerr << "locked_genSendMsgsTransaction()" << std::endl;
	std::cerr << "Generating Msg data send fron TransN: " << tr->mTransaction->transactionNumber
	          << std::endl;
#endif

	// go groups requested in transaction tr

	std::list<RsNxsItem*>::iterator lit = tr->mItems.begin();

	GxsMsgReq msgIds;
	GxsMsgResult msgs;

	if(tr->mItems.empty()){
		return;
	}

	// hacky assumes a transaction only consist of a single grpId
	RsGxsGroupId grpId;

	for(;lit != tr->mItems.end(); ++lit)
	{
		RsNxsSyncMsgItem* item = dynamic_cast<RsNxsSyncMsgItem*>(*lit);
		if (item)
		{
			msgIds[item->grpId].push_back(item->msgId);

			if(grpId.isNull())
				grpId = item->grpId;
		}
		else
		{
#ifdef NXS_NET_DEBUG
			std::cerr << "RsGxsNetService::locked_genSendMsgsTransaction(): item failed to caste to RsNxsSyncMsgItem* "
					  << std::endl;
#endif
		}
	}

	mDataStore->retrieveNxsMsgs(msgIds, msgs, false, false);

	NxsTransaction* newTr = new NxsTransaction();
	newTr->mFlag = NxsTransaction::FLAG_STATE_WAITING_CONFIRM;

	uint32_t transN = locked_getTransactionId();

	// store msg items to send in transaction
	GxsMsgResult::iterator mit = msgs.begin();
	RsPeerId peerId = tr->mTransaction->PeerId();
	uint32_t msgSize = 0;

	for(;mit != msgs.end(); ++mit)
	{
		std::vector<RsNxsMsg*>& msgV = mit->second;
		std::vector<RsNxsMsg*>::iterator vit = msgV.begin();

		for(; vit != msgV.end(); ++vit)
		{
			RsNxsMsg* msg = *vit;
			msg->PeerId(peerId);
			msg->transactionNumber = transN;
			
#ifndef 	NXS_FRAG		
			newTr->mItems.push_back(msg);
			msgSize++;
#else			
			MsgFragments fragments;
			fragmentMsg(*msg, fragments);

			MsgFragments::iterator mit = fragments.begin();

			for(; mit != fragments.end(); ++mit)
			{
				newTr->mItems.push_back(*mit);
				msgSize++;
			}
#endif			
		}
	}

	if(newTr->mItems.empty()){
		delete newTr;
		return;
	}

	uint32_t updateTS = 0;

	ServerMsgMap::const_iterator cit = mServerMsgUpdateMap.find(grpId);

	if(cit != mServerMsgUpdateMap.end())
		updateTS = cit->second->msgUpdateTS;

	RsNxsTransac* ntr = new RsNxsTransac(mServType);
	ntr->transactionNumber = transN;
	ntr->transactFlag = RsNxsTransac::FLAG_BEGIN_P1 |
			RsNxsTransac::FLAG_TYPE_MSGS;
        ntr->updateTS = updateTS;
	ntr->nItems = msgSize;
	ntr->PeerId(peerId);

	newTr->mTransaction = new RsNxsTransac(*ntr);
	newTr->mTransaction->PeerId(mOwnId);
    newTr->mTimeOut = time(NULL) + mTransactionTimeOut;

	ntr->PeerId(tr->mTransaction->PeerId());
	sendItem(ntr);

	locked_addTransaction(newTr);

	return;
}
uint32_t RsGxsNetService::locked_getTransactionId()
{
	return ++mTransactionN;
}
bool RsGxsNetService::locked_addTransaction(NxsTransaction* tr)
{
	const RsPeerId& peer = tr->mTransaction->PeerId();
	uint32_t transN = tr->mTransaction->transactionNumber;
	TransactionIdMap& transMap = mTransactions[peer];
	bool transNumExist = transMap.find(transN)
			!= transMap.end();


	if(transNumExist){
#ifdef NXS_NET_DEBUG
		std::cerr << "locked_addTransaction() " << std::endl;
		std::cerr << "Transaction number exist already, transN: " << transN
				  << std::endl;
#endif
		return false;
	}else{
		transMap[transN] = tr;
		return true;
	}
}

void RsGxsNetService::cleanTransactionItems(NxsTransaction* tr) const
{
	std::list<RsNxsItem*>::iterator lit = tr->mItems.begin();

	for(; lit != tr->mItems.end(); ++lit)
	{
		delete *lit;
	}

	tr->mItems.clear();
}

void RsGxsNetService::locked_pushGrpRespFromList(std::list<RsNxsItem*>& respList,
		const RsPeerId& peer, const uint32_t& transN)
{
	NxsTransaction* tr = new NxsTransaction();
	tr->mItems = respList;

	tr->mFlag = NxsTransaction::FLAG_STATE_WAITING_CONFIRM;
	RsNxsTransac* trItem = new RsNxsTransac(mServType);
	trItem->transactFlag = RsNxsTransac::FLAG_BEGIN_P1
			| RsNxsTransac::FLAG_TYPE_GRP_LIST_RESP;
	trItem->nItems = respList.size();
	trItem->timestamp = 0;
	trItem->PeerId(peer);
	trItem->transactionNumber = transN;
	// also make a copy for the resident transaction
	tr->mTransaction = new RsNxsTransac(*trItem);
	tr->mTransaction->PeerId(mOwnId);
    tr->mTimeOut = time(NULL) + mTransactionTimeOut;
	// signal peer to prepare for transaction
	sendItem(trItem);
	locked_addTransaction(tr);
}

bool RsGxsNetService::locked_CanReceiveUpdate(const RsNxsSyncGrp *item)
{
    // don't sync if you have no new updates for this peer
    if(mGrpServerUpdateItem)
    {
        if(item->updateTS >= mGrpServerUpdateItem->grpUpdateTS && item->updateTS != 0)
        {
#ifdef NXS_NET_DEBUG
	    std::cerr << "RsGxsNetService::locked_CanReceiveUpdate() No Updates";
	    std::cerr << std::endl;
#endif
            return false;
        }
    }

    return true;
}

void RsGxsNetService::handleRecvSyncGroup(RsNxsSyncGrp* item)
{

	RS_STACK_MUTEX(mNxsMutex) ;

        if(!locked_CanReceiveUpdate(item))
	{
#ifdef NXS_NET_DEBUG
	std::cerr << "RsGxsNetService::handleRecvSyncGroup() Cannot RecvUpdate";
	std::cerr << std::endl;
#endif
            return;
	}

	RsPeerId peer = item->PeerId();


	std::map<RsGxsGroupId, RsGxsGrpMetaData*> grp;
	mDataStore->retrieveGxsGrpMetaData(grp);

	if(grp.empty())
	{
#ifdef NXS_NET_DEBUG
		std::cerr << "RsGxsNetService::handleRecvSyncGroup() Grp Empty";
		std::cerr << std::endl;
#endif
		return;
	}

	std::map<RsGxsGroupId, RsGxsGrpMetaData*>::iterator mit =
	grp.begin();

	std::list<RsNxsItem*> itemL;

	uint32_t transN = locked_getTransactionId();

	std::vector<GrpIdCircleVet> toVet;
#ifdef NXS_NET_DEBUG
	std::cerr << "RsGxsNetService::handleRecvSyncGroup() \nService: " << mServType << "\nGroup list beings being sent: " << std::endl;
#endif

	for(; mit != grp.end(); ++mit)
	{
		RsGxsGrpMetaData* grpMeta = mit->second;

		if(grpMeta->mSubscribeFlags &
				GXS_SERV::GROUP_SUBSCRIBE_SUBSCRIBED)
		{

			// check if you can send this id to peer
			// or if you need to add to the holding
			// pen for peer to be vetted
			if(canSendGrpId(peer, *grpMeta, toVet))
			{
				RsNxsSyncGrpItem* gItem = new
					RsNxsSyncGrpItem(mServType);
				gItem->flag = RsNxsSyncGrpItem::FLAG_RESPONSE;
				gItem->grpId = mit->first;
				gItem->publishTs = mit->second->mPublishTs;
				gItem->authorId = grpMeta->mAuthorId;
				gItem->PeerId(peer);
				gItem->transactionNumber = transN;
				itemL.push_back(gItem);
#ifdef NXS_NET_DEBUG
				std::cerr << "RsGxsNetService::handleRecvSyncGroup";
				std::cerr << std::endl;
				std::cerr << "Group : " << grpMeta->mGroupName;
				std::cerr << ", id: " << gItem->grpId;
				std::cerr << ", authorId: " << gItem->authorId;
				std::cerr << std::endl;
#endif
			}
		}

		delete grpMeta; // release resource
	}

	if(!toVet.empty())
	{
		mPendingCircleVets.push_back(new GrpCircleIdRequestVetting(mCircles, mPgpUtils, toVet, peer));
	}

	locked_pushGrpRespFromList(itemL, peer, transN);

	return;
}



bool RsGxsNetService::canSendGrpId(const RsPeerId& sslId, RsGxsGrpMetaData& grpMeta, std::vector<GrpIdCircleVet>& toVet)
{
#ifdef NXS_NET_DEBUG
	std::cerr << "RsGxsNetService::canSendGrpId()";
	std::cerr << std::endl;
#endif
	// first do the simple checks
	uint8_t circleType = grpMeta.mCircleType;

	if(circleType == GXS_CIRCLE_TYPE_LOCAL)
	{
#ifdef NXS_NET_DEBUG
		std::cerr << "RsGxsNetService::canSendGrpId() LOCAL_CIRCLE, cannot send";
		std::cerr << std::endl;
#endif
		return false;
	}

	if(circleType == GXS_CIRCLE_TYPE_PUBLIC)
	{
#ifdef NXS_NET_DEBUG
		std::cerr << "RsGxsNetService::canSendGrpId() PUBLIC_CIRCLE, can send";
		std::cerr << std::endl;
#endif
		return true;
	}

	if(circleType == GXS_CIRCLE_TYPE_EXTERNAL)
	{
		const RsGxsCircleId& circleId = grpMeta.mCircleId;
		if(circleId.isNull())
		{
			std::cerr << "RsGxsNetService::canSendGrpId() ERROR; EXTERNAL_CIRCLE missing NULL CircleId: ";
			std::cerr << grpMeta.mGroupId;
			std::cerr << std::endl;

			// ERROR, will never be shared.
			return false;
		}

		if(mCircles->isLoaded(circleId))
		{
#ifdef NXS_NET_DEBUG
			std::cerr << "RsGxsNetService::canSendGrpId() EXTERNAL_CIRCLE, checking mCircles->canSend";
			std::cerr << std::endl;
#endif
			const RsPgpId& pgpId = mPgpUtils->getPGPId(sslId);
			return mCircles->canSend(circleId, pgpId);
		}

		toVet.push_back(GrpIdCircleVet(grpMeta.mGroupId, circleId, grpMeta.mAuthorId));
		return false;
	}

	if(circleType == GXS_CIRCLE_TYPE_YOUREYESONLY)
	{
#ifdef NXS_NET_DEBUG
		std::cerr << "RsGxsNetService::canSendGrpId() YOUREYESONLY, checking further";
		std::cerr << std::endl;
#endif
		// a non empty internal circle id means this
		// is the personal circle owner
		if(!grpMeta.mInternalCircle.isNull())
		{
			const RsGxsCircleId& internalCircleId = grpMeta.mInternalCircle;
#ifdef NXS_NET_DEBUG
			std::cerr << "RsGxsNetService::canSendGrpId() have mInternalCircle - we are Group creator";
			std::cerr << std::endl;
			std::cerr << "RsGxsNetService::canSendGrpId() mCircleId: " << grpMeta.mCircleId;
			std::cerr << std::endl;
			std::cerr << "RsGxsNetService::canSendGrpId() mInternalCircle: " << grpMeta.mInternalCircle;
			std::cerr << std::endl;
#endif

			if(mCircles->isLoaded(internalCircleId))
			{
#ifdef NXS_NET_DEBUG
				std::cerr << "RsGxsNetService::canSendGrpId() circle Loaded - checking mCircles->canSend";
				std::cerr << std::endl;
#endif
				const RsPgpId& pgpId = mPgpUtils->getPGPId(sslId);
				return mCircles->canSend(internalCircleId, pgpId);
			}

#ifdef NXS_NET_DEBUG
			std::cerr << "RsGxsNetService::canSendGrpId() Circle Not Loaded - add to vetting";
			std::cerr << std::endl;
#endif
			toVet.push_back(GrpIdCircleVet(grpMeta.mGroupId, internalCircleId, grpMeta.mAuthorId));
			return false;
		}
		else
		{
			// an empty internal circle id means this peer can only
			// send circle related info from peer he received it
#ifdef NXS_NET_DEBUG
			std::cerr << "RsGxsNetService::canSendGrpId() mInternalCircle not set, someone else's personal circle";
			std::cerr << std::endl;
#endif
			if(grpMeta.mOriginator == sslId)
			{
#ifdef NXS_NET_DEBUG
				std::cerr << "RsGxsNetService::canSendGrpId() Originator matches -> can send";
				std::cerr << std::endl;
#endif
				return true;
			}
			else
			{
#ifdef NXS_NET_DEBUG
				std::cerr << "RsGxsNetService::canSendGrpId() Originator doesn't match -> cannot send";
				std::cerr << std::endl;
#endif
				return false;
			}
		}
	}

	return true;
}

bool RsGxsNetService::checkCanRecvMsgFromPeer(const RsPeerId& sslId,
		const RsGxsGrpMetaData& grpMeta) {

	#ifdef NXS_NET_DEBUG
		std::cerr << "RsGxsNetService::checkCanRecvMsgFromPeer()";
		std::cerr << std::endl;
	#endif
		// first do the simple checks
		uint8_t circleType = grpMeta.mCircleType;

		if(circleType == GXS_CIRCLE_TYPE_LOCAL)
		{
	#ifdef NXS_NET_DEBUG
			std::cerr << "RsGxsNetService::checkCanRecvMsgFromPeer() LOCAL_CIRCLE, cannot request sync from peer";
			std::cerr << std::endl;
	#endif
			return false;
		}

		if(circleType == GXS_CIRCLE_TYPE_PUBLIC)
		{
	#ifdef NXS_NET_DEBUG
			std::cerr << "RsGxsNetService::checkCanRecvMsgFromPeer() PUBLIC_CIRCLE, can request msg sync";
			std::cerr << std::endl;
	#endif
			return true;
		}

		if(circleType == GXS_CIRCLE_TYPE_EXTERNAL)
		{
			const RsGxsCircleId& circleId = grpMeta.mCircleId;
			if(circleId.isNull())
			{
				std::cerr << "RsGxsNetService::checkCanRecvMsgFromPeer() ERROR; EXTERNAL_CIRCLE missing NULL CircleId";
				std::cerr << grpMeta.mGroupId;
				std::cerr << std::endl;

				// should just be shared. ? no - this happens for
				// Circle Groups which lose their CircleIds.
				// return true;
			}

			if(mCircles->isLoaded(circleId))
			{
	#ifdef NXS_NET_DEBUG
				std::cerr << "RsGxsNetService::checkCanRecvMsgFromPeer() EXTERNAL_CIRCLE, checking mCircles->canSend";
				std::cerr << std::endl;
	#endif
				const RsPgpId& pgpId = mPgpUtils->getPGPId(sslId);
				return mCircles->canSend(circleId, pgpId);
			}
			else
				mCircles->loadCircle(circleId); // simply request for next pass

			return false;
		}

		if(circleType == GXS_CIRCLE_TYPE_YOUREYESONLY) // do not attempt to sync msg unless to originator or those permitted
		{
	#ifdef NXS_NET_DEBUG
			std::cerr << "RsGxsNetService::checkCanRecvMsgFromPeer() YOUREYESONLY, checking further";
			std::cerr << std::endl;
	#endif
			// a non empty internal circle id means this
			// is the personal circle owner
			if(!grpMeta.mInternalCircle.isNull())
			{
				const RsGxsCircleId& internalCircleId = grpMeta.mInternalCircle;
	#ifdef NXS_NET_DEBUG
				std::cerr << "RsGxsNetService::canSendGrpId() have mInternalCircle - we are Group creator";
				std::cerr << std::endl;
				std::cerr << "RsGxsNetService::canSendGrpId() mCircleId: " << grpMeta.mCircleId;
				std::cerr << std::endl;
				std::cerr << "RsGxsNetService::canSendGrpId() mInternalCircle: " << grpMeta.mInternalCircle;
				std::cerr << std::endl;
	#endif

				if(mCircles->isLoaded(internalCircleId))
				{
	#ifdef NXS_NET_DEBUG
					std::cerr << "RsGxsNetService::canSendGrpId() circle Loaded - checking mCircles->canSend";
					std::cerr << std::endl;
	#endif
					const RsPgpId& pgpId = mPgpUtils->getPGPId(sslId);
					return mCircles->canSend(internalCircleId, pgpId);
				}
				else
					mCircles->loadCircle(internalCircleId); // request for next pass

				return false;
			}
			else
			{
				// an empty internal circle id means this peer can only
				// send circle related info from peer he received it
	#ifdef NXS_NET_DEBUG
				std::cerr << "RsGxsNetService::canSendGrpId() mInternalCircle not set, someone else's personal circle";
				std::cerr << std::endl;
	#endif
				if(grpMeta.mOriginator == sslId)
				{
	#ifdef NXS_NET_DEBUG
					std::cerr << "RsGxsNetService::canSendGrpId() Originator matches -> can send";
					std::cerr << std::endl;
	#endif
					return true;
				}
				else
				{
	#ifdef NXS_NET_DEBUG
					std::cerr << "RsGxsNetService::canSendGrpId() Originator doesn't match -> cannot send";
					std::cerr << std::endl;
	#endif
					return false;
				}
			}
		}

		return true;
}

bool RsGxsNetService::locked_CanReceiveUpdate(const RsNxsSyncMsg *item)
{
    ServerMsgMap::const_iterator cit = mServerMsgUpdateMap.find(item->grpId);

    if(cit != mServerMsgUpdateMap.end())
    {
        const RsGxsServerMsgUpdateItem *msui = cit->second;

        if(item->updateTS >= msui->msgUpdateTS && item->updateTS != 0)
        {
#ifdef	NXS_NET_DEBUG
        	std::cerr << "RsGxsNetService::locked_CanReceiveUpdate(): Msgs up to date" << std::endl;
#endif
            return false;
        }
    }
    return true;
}
void RsGxsNetService::handleRecvSyncMessage(RsNxsSyncMsg* item)
{
	RS_STACK_MUTEX(mNxsMutex) ;

    // We do that early, so as to get info about who sends data about which group,
    // even when the group doesn't need update.
    mGroupSuppliers[item->grpId].insert(item->PeerId()) ;

        if(!locked_CanReceiveUpdate(item))
            return;

	const RsPeerId& peer = item->PeerId();

	GxsMsgMetaResult metaResult;
	GxsMsgReq req;

	std::map<RsGxsGroupId, RsGxsGrpMetaData*> grpMetas;
	grpMetas[item->grpId] = NULL;
	mDataStore->retrieveGxsGrpMetaData(grpMetas);
	RsGxsGrpMetaData* grpMeta = grpMetas[item->grpId];

	if(grpMeta == NULL)
		return;

    req[item->grpId] = std::vector<RsGxsMessageId>();
	mDataStore->retrieveGxsMsgMetaData(req, metaResult);
	std::vector<RsGxsMsgMetaData*>& msgMetas = metaResult[item->grpId];

	if(req.empty())
	{
		delete(grpMeta);
		return;
	}

	std::list<RsNxsItem*> itemL;

	uint32_t transN = locked_getTransactionId();

	if(canSendMsgIds(msgMetas, *grpMeta, peer))
	{
		std::vector<RsGxsMsgMetaData*>::iterator vit = msgMetas.begin();

		for(; vit != msgMetas.end(); ++vit)
		{
			RsGxsMsgMetaData* m = *vit;

			RsNxsSyncMsgItem* mItem = new
			RsNxsSyncMsgItem(mServType);
			mItem->flag = RsNxsSyncGrpItem::FLAG_RESPONSE;
			mItem->grpId = m->mGroupId;
			mItem->msgId = m->mMsgId;
			mItem->authorId = m->mAuthorId;
			mItem->PeerId(peer);
			mItem->transactionNumber = transN;
			itemL.push_back(mItem);
		}

		if(!itemL.empty())
			locked_pushMsgRespFromList(itemL, peer, transN);
	}

	std::vector<RsGxsMsgMetaData*>::iterator vit = msgMetas.begin();
	// release meta resource
	for(vit = msgMetas.begin(); vit != msgMetas.end(); ++vit)
		delete *vit;

	delete(grpMeta);
}

void RsGxsNetService::locked_pushMsgRespFromList(std::list<RsNxsItem*>& itemL, const RsPeerId& sslId,
		const uint32_t& transN)
{
	NxsTransaction* tr = new NxsTransaction();
	tr->mItems = itemL;
	tr->mFlag = NxsTransaction::FLAG_STATE_WAITING_CONFIRM;
	RsNxsTransac* trItem = new RsNxsTransac(mServType);
	trItem->transactFlag = RsNxsTransac::FLAG_BEGIN_P1
			| RsNxsTransac::FLAG_TYPE_MSG_LIST_RESP;

	trItem->nItems = itemL.size();

	trItem->timestamp = 0;
	trItem->PeerId(sslId);
	trItem->transactionNumber = transN;

	// also make a copy for the resident transaction
	tr->mTransaction = new RsNxsTransac(*trItem);
	tr->mTransaction->PeerId(mOwnId);
    tr->mTimeOut = time(NULL) + mTransactionTimeOut;

	// signal peer to prepare for transaction
	sendItem(trItem);

	locked_addTransaction(tr);
}

bool RsGxsNetService::canSendMsgIds(const std::vector<RsGxsMsgMetaData*>& msgMetas,
		const RsGxsGrpMetaData& grpMeta, const RsPeerId& sslId)
{
#ifdef NXS_NET_DEBUG
	std::cerr << "RsGxsNetService::canSendMsgIds() CIRCLE VETTING";
	std::cerr << std::endl;
#endif

	// first do the simple checks
	uint8_t circleType = grpMeta.mCircleType;

	if(circleType == GXS_CIRCLE_TYPE_LOCAL)
		return false;

	if(circleType == GXS_CIRCLE_TYPE_PUBLIC)
		return true;

	const RsGxsCircleId& circleId = grpMeta.mCircleId;

	if(circleType == GXS_CIRCLE_TYPE_EXTERNAL)
	{
		if(mCircles->isLoaded(circleId))
		{
			const RsPgpId& pgpId = mPgpUtils->getPGPId(sslId);
			return mCircles->canSend(circleId, pgpId);
		}

		std::vector<MsgIdCircleVet> toVet;
		std::vector<RsGxsMsgMetaData*>::const_iterator vit = msgMetas.begin();

		for(; vit != msgMetas.end(); ++vit)
		{
			const RsGxsMsgMetaData* const& meta = *vit;

			MsgIdCircleVet mic(meta->mMsgId, meta->mAuthorId);
			toVet.push_back(mic);
		}

		if(!toVet.empty())
			mPendingCircleVets.push_back(new MsgCircleIdsRequestVetting(mCircles, mPgpUtils, toVet, grpMeta.mGroupId,
					sslId, grpMeta.mCircleId));

		return false;
	}

	if(circleType == GXS_CIRCLE_TYPE_YOUREYESONLY)
	{
		// a non empty internal circle id means this
		// is the personal circle owner
		if(!grpMeta.mInternalCircle.isNull())
		{
			const RsGxsCircleId& internalCircleId = grpMeta.mInternalCircle;
			if(mCircles->isLoaded(internalCircleId))
			{
				const RsPgpId& pgpId = mPgpUtils->getPGPId(sslId);
				return mCircles->canSend(internalCircleId, pgpId);
			}

			std::vector<MsgIdCircleVet> toVet;
			std::vector<RsGxsMsgMetaData*>::const_iterator vit = msgMetas.begin();

			for(; vit != msgMetas.end(); ++vit)
			{
				const RsGxsMsgMetaData* const& meta = *vit;

				MsgIdCircleVet mic(meta->mMsgId, meta->mAuthorId);
				toVet.push_back(mic);
			}

			if(!toVet.empty())
				mPendingCircleVets.push_back(new MsgCircleIdsRequestVetting(mCircles, mPgpUtils, 
						toVet, grpMeta.mGroupId,
						sslId, grpMeta.mCircleId));

			return false;
		}
		else
		{
			// an empty internal circle id means this peer can only
			// send circle related info from peer he received it
			if(grpMeta.mOriginator == sslId)
				return true;
			else
				return false;
		}
	}

	return true;
}

/** inherited methods **/

void RsGxsNetService::pauseSynchronisation(bool /* enabled */)
{

}

void RsGxsNetService::setSyncAge(uint32_t /* age */)
{

}

int RsGxsNetService::requestGrp(const std::list<RsGxsGroupId>& grpId, const RsPeerId& peerId)
{
	RS_STACK_MUTEX(mNxsMutex) ;
	mExplicitRequest[peerId].assign(grpId.begin(), grpId.end());
	return 1;
}

void RsGxsNetService::processExplicitGroupRequests()
{
	RS_STACK_MUTEX(mNxsMutex) ;

	std::map<RsPeerId, std::list<RsGxsGroupId> >::const_iterator cit = mExplicitRequest.begin();

	for(; cit != mExplicitRequest.end(); ++cit)
	{
		const RsPeerId& peerId = cit->first;
		const std::list<RsGxsGroupId>& groupIdList = cit->second;

		std::list<RsNxsItem*> grpSyncItems;
		std::list<RsGxsGroupId>::const_iterator git = groupIdList.begin();
		uint32_t transN = locked_getTransactionId();
		for(; git != groupIdList.end(); ++git)
		{
			RsNxsSyncGrpItem* item = new RsNxsSyncGrpItem(mServType);
			item->grpId = *git;
			item->PeerId(peerId);
			item->flag = RsNxsSyncGrpItem::FLAG_REQUEST;
			item->transactionNumber = transN;
			grpSyncItems.push_back(item);
		}

		if(!grpSyncItems.empty())
			locked_pushGrpTransactionFromList(grpSyncItems, peerId, transN);
	}

	mExplicitRequest.clear();
}

#define NXS_NET_DEBUG
int RsGxsNetService::sharePublishKey(const RsGxsGroupId& grpId,const std::list<RsPeerId>& peers)
{
	RS_STACK_MUTEX(mNxsMutex) ;

	mPendingPublishKeyRecipients[grpId] = peers ;

    std::cerr << "RsGxsNetService::sharePublishKeys() " << (void*)this << " adding publish keys for grp " << grpId << " to sending list" << std::endl;

    return true ;
}

void RsGxsNetService::sharePublishKeysPending()
{
	RS_STACK_MUTEX(mNxsMutex) ;

    if(mPendingPublishKeyRecipients.empty())
        return ;

#ifdef NXS_NET_DEBUG
    std::cerr << "RsGxsNetService::sharePublishKeys()  " << (void*)this << std::endl;
#endif
    // get list of peers that are online

    std::set<RsPeerId> peersOnline;
    std::list<RsGxsGroupId> toDelete;
    std::map<RsGxsGroupId,std::list<RsPeerId> >::iterator mit ;

    mNetMgr->getOnlineList(mServiceInfo.mServiceType, peersOnline);

#ifdef NXS_NET_DEBUG
    std::cerr << "  " << peersOnline.size() << " peers online." << std::endl;
#endif
    /* send public key to peers online */

    for(mit = mPendingPublishKeyRecipients.begin();  mit != mPendingPublishKeyRecipients.end(); ++mit)
    {
        // Compute the set of peers to send to. We start with this, to avoid retrieving the data for nothing.

        std::list<RsPeerId> recipients ;
        std::list<RsPeerId> offline_recipients ;

        for(std::list<RsPeerId>::const_iterator it(mit->second.begin());it!=mit->second.end();++it)
            if(peersOnline.find(*it) != peersOnline.end())
            {
#ifdef NXS_NET_DEBUG
                std::cerr << "    " << *it << ": online. Adding." << std::endl;
#endif
                recipients.push_back(*it) ;
            }
            else
            {
#ifdef NXS_NET_DEBUG
                std::cerr << "    " << *it << ": offline. Keeping for next try." << std::endl;
#endif
                offline_recipients.push_back(*it) ;
            }

        // If empty, skip

        if(recipients.empty())
        {
            std::cerr << "  No recipients online. Skipping." << std::endl;
            continue ;
        }

        // Get the meta data for this group Id
        //
        std::map<RsGxsGroupId, RsGxsGrpMetaData*> grpMetaMap;
        grpMetaMap[mit->first] = NULL;
        mDataStore->retrieveGxsGrpMetaData(grpMetaMap);

        // Find the publish keys in the retrieved info

        RsGxsGrpMetaData *grpMeta = grpMetaMap[mit->first] ;

        if(grpMeta == NULL)
        {
            std::cerr << "(EE) RsGxsNetService::sharePublishKeys() Publish keys cannot be found for group " << mit->first << std::endl;
            continue ;
        }

        const RsTlvSecurityKeySet& keys = grpMeta->keys;

        std::map<RsGxsId, RsTlvSecurityKey>::const_iterator kit = keys.keys.begin(), kit_end = keys.keys.end();
        bool publish_key_found = false;
        RsTlvSecurityKey publishKey ;

        for(; kit != kit_end && !publish_key_found; ++kit)
        {
            publish_key_found = (kit->second.keyFlags == (RSTLV_KEY_DISTRIB_PRIVATE | RSTLV_KEY_TYPE_FULL));
            publishKey = kit->second ;
        }

        if(!publish_key_found)
        {
            std::cerr << "(EE) no publish key in group " << mit->first << ". Cannot share!" << std::endl;
            continue ;
        }


#ifdef NXS_NET_DEBUG
        std::cerr << "  using publish key ID=" << publishKey.keyId << ", flags=" << publishKey.keyFlags << std::endl;
#endif
        for(std::list<RsPeerId>::const_iterator it(recipients.begin());it!=recipients.end();++it)
        {
            /* Create publish key sharing item */
            RsNxsGroupPublishKeyItem *publishKeyItem = new RsNxsGroupPublishKeyItem(mServType);

            publishKeyItem->clear();
            publishKeyItem->grpId = mit->first;

            publishKeyItem->key = publishKey ;
            publishKeyItem->PeerId(*it);

            sendItem(publishKeyItem);
#ifdef NXS_NET_DEBUG
            std::cerr << "  sent key item to " << *it << std::endl;
#endif
        }

        mit->second = offline_recipients ;

        // If given peers have all received key(s) then stop sending for group
        if(offline_recipients.empty())
            toDelete.push_back(mit->first);
    }

    // delete pending peer list which are done with
    for(std::list<RsGxsGroupId>::const_iterator lit = toDelete.begin(); lit != toDelete.end(); ++lit)
        mPendingPublishKeyRecipients.erase(*lit);
}

void RsGxsNetService::handleRecvPublishKeys(RsNxsGroupPublishKeyItem *item)
{
#ifdef NXS_NET_DEBUG
    std::cerr << "RsGxsNetService::sharePublishKeys() " << std::endl;
#endif
	RS_STACK_MUTEX(mNxsMutex) ;

#ifdef NXS_NET_DEBUG
	 std::cerr << "  PeerId : " << item->PeerId() << std::endl;
	 std::cerr << "  GrpId: " << item->grpId << std::endl;
     std::cerr << "  Got key Item: " << item->key.keyId << std::endl;
#endif

	 // Get the meta data for this group Id
	 //
	 std::map<RsGxsGroupId, RsGxsGrpMetaData*> grpMetaMap;
	 grpMetaMap[item->grpId] = NULL;
	 mDataStore->retrieveGxsGrpMetaData(grpMetaMap);

	 // update the publish keys in this group meta info

	 RsGxsGrpMetaData *grpMeta = grpMetaMap[item->grpId] ;

	 // Check that the keys correspond, and that FULL keys are supplied, etc.

	 std::cerr << "  Key received: " << std::endl;

	 bool admin = (item->key.keyFlags & RSTLV_KEY_DISTRIB_ADMIN)   && (item->key.keyFlags & RSTLV_KEY_TYPE_FULL) ;
	 bool publi = (item->key.keyFlags & RSTLV_KEY_DISTRIB_PRIVATE) && (item->key.keyFlags & RSTLV_KEY_TYPE_FULL) ;

     std::cerr << "    Key id = " << item->key.keyId << "  admin=" << admin << ", publish=" << publi << " ts=" << item->key.endTS << std::endl;

     if(!(!admin && publi))
     {
         std::cerr << "  Key is not a publish private key. Discarding!" << std::endl;
         return ;
     }
	 // Also check that we don't already have full keys for that group.
	 
     std::map<RsGxsId,RsTlvSecurityKey>::iterator it = grpMeta->keys.keys.find(item->key.keyId) ;

     if(it == grpMeta->keys.keys.end())
     {
         std::cerr << "   (EE) Key not found in known group keys. This is an inconsistency." << std::endl;
         return ;
     }

     if((it->second.keyFlags & RSTLV_KEY_DISTRIB_PRIVATE) && (it->second.keyFlags & RSTLV_KEY_TYPE_FULL))
     {
         std::cerr << "   (EE) Publish key already present in database. Discarding message." << std::endl;
			return ;
     }

	  // Store/update the info.

     it->second = item->key ;
     bool ret = mDataStore->updateGroupKeys(item->grpId,grpMeta->keys, grpMeta->mSubscribeFlags | GXS_SERV::GROUP_SUBSCRIBE_PUBLISH) ;

	 if(!ret)
		 std::cerr << "(EE) could not update database. Something went wrong." << std::endl;
#ifdef NXS_NET_DEBUG
	 else
		 std::cerr << "  updated database with new publish keys." << std::endl;
#endif
}

