/****************************************************************
 *  RetroShare is distributed under the following license:
 *
 *  Copyright (C) 2006,  crypton
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

#include "ConfCertDialog.h"

#include <QMessageBox>
#include <QDateTime>
#include <QMenu>
#include <QClipboard>
#include <QMap>

#include <iostream>

#include <retroshare/rspeers.h>
#include <retroshare/rsdisc.h>
#include <retroshare/rsmsgs.h>

#include "gui/help/browser/helpbrowser.h"
#include "gui/common/PeerDefs.h"
#include "gui/common/StatusDefs.h"
#include "gui/RetroShareLink.h"
#include "gui/notifyqt.h"
#include "gui/common/AvatarDefs.h"
#include "gui/MainWindow.h"
#include "mainpage.h"
#include "util/DateTime.h"
#include "util/misc.h"

static QMap<RsPeerId, ConfCertDialog*> instances_ssl;
static QMap<RsPgpId, ConfCertDialog*> instances_pgp;

ConfCertDialog *ConfCertDialog::instance(const RsPeerId& peer_id)
{

    ConfCertDialog *d = instances_ssl[peer_id];
    if (d) {
        return d;
    }

    RsPeerDetails details ;
    if(!rsPeers->getPeerDetails(peer_id,details))
        return NULL ;

    d = new ConfCertDialog(peer_id,details.gpg_id);
    instances_ssl[peer_id] = d;

    return d;
}
ConfCertDialog *ConfCertDialog::instance(const RsPgpId& pgp_id)
{

    ConfCertDialog *d = instances_pgp[pgp_id];
    if (d) {
        return d;
    }

    d = new ConfCertDialog(RsPeerId(),pgp_id);
    instances_pgp[pgp_id] = d;

    return d;
}
/** Default constructor */
ConfCertDialog::ConfCertDialog(const RsPeerId& id, const RsPgpId &pgp_id, QWidget *parent, Qt::WindowFlags flags)
  : QDialog(parent, flags), peerId(id), pgpId(pgp_id)
{
    /* Invoke Qt Designer generated QObject setup routine */
    ui.setupUi(this);

//	 if(id.isNull())
//		 ui._useOldFormat_CB->setChecked(true) ;
//	 else
//	 {
//		 ui._useOldFormat_CB->setChecked(false) ;
//		 ui._useOldFormat_CB->setEnabled(false) ;
//	 }

	ui.headerFrame->setHeaderImage(QPixmap(":/images/user/identityinfo64.png"));
	ui.headerFrame->setHeaderText(tr("Friend Details"));

    //ui._chat_CB->hide() ;

	setAttribute(Qt::WA_DeleteOnClose, true);

    connect(ui.buttonBox, SIGNAL(accepted()), this, SLOT(applyDialog()));
    connect(ui.buttonBox, SIGNAL(rejected()), this, SLOT(close()));
    connect(ui.make_friend_button, SIGNAL(clicked()), this, SLOT(makeFriend()));
    connect(ui.denyFriendButton, SIGNAL(clicked()), this, SLOT(denyFriend()));
    connect(ui.signKeyButton, SIGNAL(clicked()), this, SLOT(signGPGKey()));
    connect(ui.trusthelpButton, SIGNAL(clicked()), this, SLOT(showHelpDialog()));
    connect(ui._shouldAddSignatures_CB, SIGNAL(toggled(bool)), this, SLOT(loadInvitePage()));
    connect(ui._shouldAddSignatures_CB_2, SIGNAL(toggled(bool)), this, SLOT(loadInvitePage()));

    ui.avatar->setFrameType(AvatarWidget::NORMAL_FRAME);

    MainWindow *w = MainWindow::getInstance();
    if (w) {
        connect(this, SIGNAL(configChanged()), w->getPage(MainWindow::Network), SLOT(insertConnect()));
    }
}

ConfCertDialog::~ConfCertDialog()
{
//    if(peerId.isNull())
    {
        QMap<RsPeerId, ConfCertDialog*>::iterator it = instances_ssl.find(peerId);
        if (it != instances_ssl.end())
            instances_ssl.erase(it);
    }
//    else
    {
        QMap<RsPgpId, ConfCertDialog*>::iterator it = instances_pgp.find(pgpId);
        if (it != instances_pgp.end())
            instances_pgp.erase(it);
    }
}


void ConfCertDialog::setServiceFlags()
{
    ServicePermissionFlags flags(0) ;

    if(  ui._direct_transfer_CB->isChecked()) flags = flags | RS_NODE_PERM_DIRECT_DL ;
    if(  ui._allow_push_CB->isChecked()) flags = flags | RS_NODE_PERM_ALLOW_PUSH ;

    rsPeers->setServicePermissionFlags(pgpId,flags) ;
}

void ConfCertDialog::loadAll()
{
    for(QMap<RsPeerId, ConfCertDialog*>::iterator it = instances_ssl.begin(); it != instances_ssl.end(); ++it)  it.value()->load();
    for(QMap<RsPgpId , ConfCertDialog*>::iterator it = instances_pgp.begin(); it != instances_pgp.end(); ++it)  it.value()->load();
}

void ConfCertDialog::load()
{
    RsPeerDetails detail;

    if(!(rsPeers->getPeerDetails(peerId, detail) || rsPeers->getGPGDetails(pgpId, detail)))
    {
        QMessageBox::information(this,
                                 tr("RetroShare"),
                                 tr("Error : cannot get peer details."));
        close();
        return;
    }

    if(detail.isOnlyGPGdetail && !rsPeers->isKeySupported(pgpId))
	 {
		 ui.make_friend_button->setEnabled(false) ;
         ui.make_friend_button->setToolTip(tr("The supplied key algorithm is not supported by RetroShare\n(Only RSA keys are supported at the moment)")) ;
	 }
	 else
	 {
         ui.make_friend_button->setEnabled(true) ;
		 ui.make_friend_button->setToolTip("") ;
	 }


     ui._direct_transfer_CB->setChecked(  detail.service_perm_flags & RS_NODE_PERM_DIRECT_DL ) ;
     ui._allow_push_CB->setChecked(  detail.service_perm_flags & RS_NODE_PERM_ALLOW_PUSH) ;

    ui.name->setText(QString::fromUtf8(detail.name.c_str()));
    ui.peerid->setText(QString::fromStdString(detail.id.toStdString()));

    RetroShareLink link;
    link.createPerson(detail.gpg_id);

    ui.rsid->setText(link.toHtml());
    ui.pgpfingerprint->setText(misc::fingerPrintStyleSplit(QString::fromStdString(detail.fpr.toStdString())));
    ui.rsid->setToolTip(link.title());

    if (!detail.isOnlyGPGdetail) 
	 {
         ui.avatar->setId(ChatId(peerId));

		 ui.loc->setText(QString::fromUtf8(detail.location.c_str()));
		 // Dont Show a timestamp in RS calculate the day
		 ui.lastcontact->setText(DateTime::formatLongDateTime(detail.lastConnect));

		 /* set retroshare version */
		 std::string version;
		 rsDisc->getPeerVersion(detail.id, version);
		 ui.version->setText(QString::fromStdString(version));

		 RsPeerCryptoParams cdet ;
		 if(RsControl::instance()->getPeerCryptoDetails(detail.id,cdet) && cdet.connexion_state!=0)
		 {
			 QString ct ;
			 ct += QString::fromStdString(cdet.cipher_name) ;
			 ct += QString::number(cdet.cipher_bits_1) ;
			 ct += "-"+QString::fromStdString(cdet.cipher_version) ;
			 ui.crypto_info->setText(ct) ;
		 }
		 else
			 ui.crypto_info->setText(tr("Not connected")) ;

		 if (detail.isHiddenNode)
		 {
			 /* set local address */
			 ui.localAddress->setText("hidden");
			 ui.localPort -> setValue(0);
			 /* set the server address */
			 ui.extAddress->setText("hidden");
			 ui.extPort -> setValue(0);

			 ui.dynDNS->setText(QString::fromStdString(detail.hiddenNodeAddress));
		 }
		 else
		 {
			 /* set local address */
			 ui.localAddress->setText(QString::fromStdString(detail.localAddr));
			 ui.localPort -> setValue(detail.localPort);
			 /* set the server address */
			 ui.extAddress->setText(QString::fromStdString(detail.extAddr));
			 ui.extPort -> setValue(detail.extPort);

			 ui.dynDNS->setText(QString::fromStdString(detail.dyndns));
		 }

		 ui.statusline->setText(StatusDefs::connectStateString(detail));

		 ui.ipAddressList->clear();
		 for(std::list<std::string>::const_iterator it(detail.ipAddressList.begin());it!=detail.ipAddressList.end();++it)
			 ui.ipAddressList->addItem(QString::fromStdString(*it));

		 ui.loc->show();
		 ui.label_loc->show();
		 ui.statusline->show();
		 ui.label_status->show();
		 ui.lastcontact->show();
		 ui.label_last_contact->show();
		 ui.version->show();
		 ui.label_version->show();

		 ui.groupBox->show();
		 ui.groupBox_4->show();
		 ui.tabWidget->show();
		 ui.rsid->hide();
		 ui.label_rsid->hide();
		 ui.pgpfingerprint->show();
		 ui.pgpfingerprint_label->show();

		  ui.stabWidget->setTabEnabled(2,true) ;
        ui.stabWidget->setTabEnabled(3,true) ;
          ui.stabWidget->setTabEnabled(4,true) ;
	 } 
	 else 
	 {
        //ui.avatar->setId(pgpId.toStdString(), true);

        ui.rsid->show();
        ui.peerid->hide();
        ui.label_id->hide();
        ui.label_rsid->show();
        ui.pgpfingerprint->show();
        ui.pgpfingerprint_label->show();
        ui.loc->hide();
        ui.label_loc->hide();
        ui.statusline->hide();
        ui.label_status->hide();
        ui.lastcontact->hide();
        ui.label_last_contact->hide();
        ui.version->hide();
        ui.label_version->hide();
        ui.groupBox_4->hide();
        ui.crypto_info->hide();
        ui.crypto_label->hide();

        ui.groupBox->hide();
        ui.tabWidget->hide();

		  ui.stabWidget->setTabEnabled(2,true) ;
        ui.stabWidget->setTabEnabled(3,false) ;
          ui.stabWidget->setTabEnabled(4,false) ;
          //ui._useOldFormat_CB->setEnabled(false) ;
    }

    if (detail.gpg_id == rsPeers->getGPGOwnId()) {
        ui.make_friend_button->hide();
        ui.signGPGKeyCheckBox->hide();
        ui.signKeyButton->hide();
        ui.denyFriendButton->hide();

        ui.web_of_trust_label->hide();
        ui.radioButton_trust_fully->hide();
        ui.radioButton_trust_marginnaly->hide();
        ui.radioButton_trust_never->hide();

        ui.is_signing_me->hide();
        ui.signersBox->setTitle(tr("My key is signed by : "));

    } else {
        ui.web_of_trust_label->show();
        ui.radioButton_trust_fully->show();
        ui.radioButton_trust_marginnaly->show();
        ui.radioButton_trust_never->show();

        ui.is_signing_me->show();
        ui.signersBox->setTitle(tr("Peer key is signed by : "));

        if (detail.accept_connection) {
            ui.make_friend_button->hide();
            ui.denyFriendButton->show();
            ui.signGPGKeyCheckBox->hide();
            //connection already accepted, propose to sign gpg key
            if (!detail.ownsign) {
                ui.signKeyButton->show();
            } else {
                ui.signKeyButton->hide();
            }
        } else {
            ui.make_friend_button->show();
            ui.denyFriendButton->hide();
            ui.signKeyButton->hide();
            if (!detail.ownsign) {
                ui.signGPGKeyCheckBox->show();
                ui.signGPGKeyCheckBox->setChecked(false);
            } else {
                ui.signGPGKeyCheckBox->hide();
            }
        }

        //web of trust
        if (detail.trustLvl == RS_TRUST_LVL_ULTIMATE) {
            //trust is ultimate, it means it's one of our own keys
            ui.web_of_trust_label->setText(tr("Your trust in this peer is ultimate, it's probably a key you own."));
            ui.radioButton_trust_fully->hide();
            ui.radioButton_trust_marginnaly->hide();
            ui.radioButton_trust_never->hide();
        } else {
            ui.radioButton_trust_fully->show();
            ui.radioButton_trust_marginnaly->show();
            ui.radioButton_trust_never->show();
            if (detail.trustLvl == RS_TRUST_LVL_FULL) {
                ui.web_of_trust_label->setText(tr("Your trust in this peer is full."));
                ui.radioButton_trust_fully->setChecked(true);
                ui.radioButton_trust_fully->setIcon(QIcon(":/images/security-high-48.png"));
                ui.radioButton_trust_marginnaly->setIcon(QIcon(":/images/security-medium-off-48.png"));
                ui.radioButton_trust_never->setIcon(QIcon(":/images/security-low-off-48.png"));
            } else if (detail.trustLvl == RS_TRUST_LVL_MARGINAL) {
                ui.web_of_trust_label->setText(tr("Your trust in this peer is marginal."));
                ui.radioButton_trust_marginnaly->setChecked(true);
                ui.radioButton_trust_marginnaly->setIcon(QIcon(":/images/security-medium-48.png"));
                ui.radioButton_trust_never->setIcon(QIcon(":/images/security-low-off-48.png"));
                ui.radioButton_trust_fully->setIcon(QIcon(":/images/security-high-off-48.png"));
            } else if (detail.trustLvl == RS_TRUST_LVL_NEVER) {
                ui.web_of_trust_label->setText(tr("Your trust in this peer is none."));
                ui.radioButton_trust_never->setChecked(true);
                ui.radioButton_trust_never->setIcon(QIcon(":/images/security-low-48.png"));
                ui.radioButton_trust_fully->setIcon(QIcon(":/images/security-high-off-48.png"));
                ui.radioButton_trust_marginnaly->setIcon(QIcon(":/images/security-medium-off-48.png"));
            } else {
                ui.web_of_trust_label->setText(tr("Your trust in this peer is not set."));

                //we have to set up the set exclusive to false in order to uncheck it all
                ui.radioButton_trust_fully->setAutoExclusive(false);
                ui.radioButton_trust_marginnaly->setAutoExclusive(false);
                ui.radioButton_trust_never->setAutoExclusive(false);

                ui.radioButton_trust_fully->setChecked(false);
                ui.radioButton_trust_marginnaly->setChecked(false);
                ui.radioButton_trust_never->setChecked(false);

                ui.radioButton_trust_fully->setAutoExclusive(true);
                ui.radioButton_trust_marginnaly->setAutoExclusive(true);
                ui.radioButton_trust_never->setAutoExclusive(true);

                ui.radioButton_trust_never->setIcon(QIcon(":/images/security-low-off-48.png"));
                ui.radioButton_trust_fully->setIcon(QIcon(":/images/security-high-off-48.png"));
                ui.radioButton_trust_marginnaly->setIcon(QIcon(":/images/security-medium-off-48.png"));
            }
        }

        if (detail.hasSignedMe) {
            ui.is_signing_me->setText(tr("Peer has authenticated me as a friend and did sign my PGP key"));
        } else {
            ui.is_signing_me->setText(tr("Peer has not authenticated me as a friend and did not sign my PGP key"));
        }
    }

    QString text;
    for(std::list<RsPgpId>::const_iterator it(detail.gpgSigners.begin());it!=detail.gpgSigners.end();++it) {
        link.createPerson(*it);
        if (link.valid()) {
            text += link.toHtml() + "<BR>";
        }
    }
    ui.signers->setHtml(text);

	 loadInvitePage() ;
}

void ConfCertDialog::loadInvitePage()
{
	RsPeerDetails detail;
    if (!rsPeers->getPeerDetails(peerId, detail) && !rsPeers->getGPGDetails(pgpId,detail))
    {
        QMessageBox::information(this,
                                 tr("RetroShare"),
                                 tr("Error : cannot get peer details."));
        close();
        return;
    }
     std::string pgp_key = rsPeers->getPGPKey(detail.gpg_id,ui._shouldAddSignatures_CB_2->isChecked()) ; // this needs to be a SSL id

    ui.userCertificateText_2->setReadOnly(true);
    ui.userCertificateText_2->setMinimumHeight(200);
    ui.userCertificateText_2->setMinimumWidth(530);
    QFont font("Courier New",10,50,false);
    font.setStyleHint(QFont::TypeWriter,QFont::PreferMatch);
    font.setStyle(QFont::StyleNormal);
    ui.userCertificateText_2->setFont(font);
    ui.userCertificateText_2->setText(QString::fromUtf8(pgp_key.c_str()));

     if(!detail.isOnlyGPGdetail)
     {
         std::string invite = rsPeers->GetRetroshareInvite(detail.id,ui._shouldAddSignatures_CB->isChecked()) ; // this needs to be a SSL id

         ui.userCertificateText->setReadOnly(true);
         ui.userCertificateText->setMinimumHeight(200);
         ui.userCertificateText->setMinimumWidth(530);
         QFont font("Courier New",10,50,false);
         font.setStyleHint(QFont::TypeWriter,QFont::PreferMatch);
         font.setStyle(QFont::StyleNormal);
         ui.userCertificateText->setFont(font);
         ui.userCertificateText->setText(QString::fromUtf8(invite.c_str()));
     }

}


void ConfCertDialog::applyDialog()
{
    std::cerr << "ConfCertDialog::applyDialog() called" << std::endl ;
    RsPeerDetails detail;
    if (!rsPeers->getPeerDetails(peerId, detail))
    {
        if (!rsPeers->getGPGDetails(pgpId, detail)) {
            QMessageBox::information(this,
                                     tr("RetroShare"),
                                     tr("Error : cannot get peer details."));
            close();
            return;
        }
    }

    //check the GPG trustlvl
    if (ui.radioButton_trust_fully->isChecked() && detail.trustLvl != RS_TRUST_LVL_FULL) {
        //trust has changed to fully
        rsPeers->trustGPGCertificate(pgpId, RS_TRUST_LVL_FULL);
    } else if (ui.radioButton_trust_marginnaly->isChecked() && detail.trustLvl != RS_TRUST_LVL_MARGINAL) {
        rsPeers->trustGPGCertificate(pgpId, RS_TRUST_LVL_MARGINAL);
    } else if (ui.radioButton_trust_never->isChecked() && detail.trustLvl != RS_TRUST_LVL_NEVER) {
        rsPeers->trustGPGCertificate(pgpId, RS_TRUST_LVL_NEVER);
    }

    if (!detail.isOnlyGPGdetail) {
        /* check if the data is the same */
        bool localChanged = false;
        bool extChanged = false;
        bool dnsChanged = false;

        /* set local address */
        if ((detail.localAddr != ui.localAddress->text().toStdString()) || (detail.localPort != ui.localPort -> value()))
            localChanged = true;

        if ((detail.extAddr != ui.extAddress->text().toStdString()) || (detail.extPort != ui.extPort -> value()))
            extChanged = true;

        if ((detail.dyndns != ui.dynDNS->text().toStdString()))
            dnsChanged = true;

        /* now we can action the changes */
        if (localChanged)
            rsPeers->setLocalAddress(peerId, ui.localAddress->text().toStdString(), ui.localPort->value());

        if (extChanged)
            rsPeers->setExtAddress(peerId,ui.extAddress->text().toStdString(), ui.extPort->value());

        if (dnsChanged)
            rsPeers->setDynDNS(peerId, ui.dynDNS->text().toStdString());

        if(localChanged || extChanged || dnsChanged)
            emit configChanged();
    }

	 setServiceFlags() ;

    loadAll();
    close();
}

void ConfCertDialog::makeFriend()
{
    if (ui.signGPGKeyCheckBox->isChecked()) {
        rsPeers->signGPGCertificate(pgpId);
    } 
	
    rsPeers->addFriend(peerId, pgpId);
	 setServiceFlags() ;
    loadAll();

    emit configChanged();
}

void ConfCertDialog::denyFriend()
{
    rsPeers->removeFriend(pgpId);
    loadAll();

    emit configChanged();
}

void ConfCertDialog::signGPGKey()
{
    if (!rsPeers->signGPGCertificate(pgpId)) {
                 QMessageBox::warning ( NULL,
                                tr("Signature Failure"),
                                tr("Maybe password is wrong"),
                                QMessageBox::Ok);
    }
    loadAll();

    emit configChanged();
}

/** Displays the help browser and displays the most recently viewed help
 * topic. */
void ConfCertDialog::showHelpDialog()
{
    showHelpDialog("trust");
}

/**< Shows the help browser and displays the given help <b>topic</b>. */
void ConfCertDialog::showHelpDialog(const QString &topic)
{
    HelpBrowser::showWindow(topic);
}
