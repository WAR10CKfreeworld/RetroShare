/*
 * Retroshare Posted Plugin.
 *
 * Copyright 2012-2012 by Robert Fernie.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License Version 2.1 as published by the Free Software Foundation.
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

#ifndef MRK_POSTED_POSTED_ITEM_H
#define MRK_POSTED_POSTED_ITEM_H

#include <QMetaType>

#include <retroshare/rsposted.h>
#include "gui/gxs/GxsFeedItem.h"

namespace Ui {
class PostedItem;
}

class RsPostedPost;

class PostedItem : public GxsFeedItem
{
	Q_OBJECT

public:
	PostedItem(FeedHolder *parent, uint32_t feedId, const RsGxsGroupId &groupId, const RsGxsMessageId &messageId, bool isHome, bool autoUpdate);
	PostedItem(FeedHolder *parent, uint32_t feedId, const RsPostedGroup &group, const RsPostedPost &post, bool isHome, bool autoUpdate);
	PostedItem(FeedHolder *parent, uint32_t feedId, const RsPostedPost &post, bool isHome, bool autoUpdate);
	virtual ~PostedItem();

	bool setGroup(const RsPostedGroup& group, bool doFill = true);
	bool setPost(const RsPostedPost& post, bool doFill = true);

	const RsPostedPost &getPost() const;
	RsPostedPost &post();

	/* FeedItem */
	virtual void expand(bool /*open*/) {}

private slots:
	void loadComments();
	void makeUpVote();
	void makeDownVote();
	void readToggled(bool checked);
	void readAndClearItem();

signals:
	void vote(const RsGxsGrpMsgIdPair& msgId, bool up);

protected:
	/* GxsGroupFeedItem */
	virtual QString groupName();
	virtual void loadGroup(const uint32_t &token);
	virtual RetroShareLink::enumType getLinkType() { return RetroShareLink::TYPE_UNKNOWN; }

	/* GxsFeedItem */
	virtual void loadMessage(const uint32_t &token);
	virtual QString messageName();

private:
	void setup();
	void fill();
	void setReadStatus(bool isNew, bool isUnread);

private:
	bool mInFill;

	RsPostedGroup mGroup;
	RsPostedPost mPost;

	/** Qt Designer generated object */
	Ui::PostedItem *ui;
};

Q_DECLARE_METATYPE(RsPostedPost)

#endif
