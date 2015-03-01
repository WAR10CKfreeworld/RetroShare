/****************************************************************
 *  RetroShare is distributed under the following license:
 *
 *  Copyright (C) 2006, crypton
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, 
 *  Boston, MA  02110-1301, USA.
 ****************************************************************/


#ifndef _CHATLOBBYDIALOG_H
#define _CHATLOBBYDIALOG_H

#include "ui_ChatLobbyDialog.h"
#include "gui/common/RSTreeWidgetItem.h"
#include "ChatDialog.h"

class QToolButton;

class ChatLobbyDialog: public ChatDialog
{
	Q_OBJECT 

	friend class ChatDialog;

public:
    void displayLobbyEvent(int event_type, const RsGxsId &gxs_id, const QString& str);

	virtual void showDialog(uint chatflags);
	virtual ChatWidget *getChatWidget();
	virtual bool hasPeerStatus() { return false; }
	virtual bool notifyBlink();
    void setIdentity(const RsGxsId& gxs_id);
    bool isParticipantMuted(const RsGxsId &participant);
	ChatLobbyId id() const { return lobbyId ;}

private slots:
	void showParticipantsFrame(bool show);
	void participantsTreeWidgetCustomPopupMenu( QPoint point );
	void inviteFriends() ;
	void leaveLobby() ;

signals:
	void lobbyLeave(ChatLobbyId) ;
	void typingEventReceived(ChatLobbyId) ;
	void messageReceived(ChatLobbyId) ;
	void peerJoined(ChatLobbyId) ;
	void peerLeft(ChatLobbyId) ;

protected:
	/** Default constructor */
	ChatLobbyDialog(const ChatLobbyId& lid, QWidget *parent = 0, Qt::WindowFlags flags = 0);

	/** Default destructor */
	virtual ~ChatLobbyDialog();

	void processSettings(bool load);
    virtual void init();
	virtual bool canClose();
    virtual void addChatMsg(const ChatMessage &msg);

protected slots:
    //void changeNickname();
	void changePartipationState();
	void participantsTreeWidgetDoubleClicked(QTreeWidgetItem *item, int column);

private:
	void updateParticipantsList();

    void muteParticipant(const RsGxsId& nickname);
    void unMuteParticipant(const RsGxsId& nickname);
    bool isNicknameInLobby(const RsGxsId& nickname);
	
	ChatLobbyId lobbyId;
	QString _lobby_name ;
	time_t lastUpdateListTime;

        RSTreeWidgetItemCompareRole *mParticipantCompareRole ;

    QToolButton *inviteFriendsButton ;
	QToolButton *unsubscribeButton ;

	/** Qt Designer generated object */
	Ui::ChatLobbyDialog ui;
	
	/** Ignored Users in Chatlobby by nickname until we had implemented Peer Ids in ver 0.6 */
    std::set<RsGxsId> mutedParticipants;

	QAction *muteAct;
};

#endif
