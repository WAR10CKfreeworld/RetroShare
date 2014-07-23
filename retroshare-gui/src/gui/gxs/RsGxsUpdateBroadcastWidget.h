#pragma once

#include <QWidget>
#include <retroshare/rsgxsifacetypes.h>

// This class implement a basic RS functionality which is that widgets displaying info
// should update regularly. They also should update only when visible, to save CPU time.
//
// Using this class simply needs to derive your widget from RsGxsUpdateBroadcastWidget
// and oveload the updateDisplay() function with the actual code that updates the
// widget.
//

class RsGxsIfaceHelper;
class RsGxsUpdateBroadcastBase;

class RsGxsUpdateBroadcastWidget : public QWidget
{
	Q_OBJECT

public:
	RsGxsUpdateBroadcastWidget(RsGxsIfaceHelper* ifaceImpl, QWidget *parent = NULL, Qt::WindowFlags flags = 0);
	virtual ~RsGxsUpdateBroadcastWidget();

	void fillComplete();
	void setUpdateWhenInvisible(bool update);
	const std::list<RsGxsGroupId> &getGrpIds();
	const std::list<RsGxsGroupId> &getGrpIdsMeta();
	void getAllGrpIds(std::list<RsGxsGroupId> &grpIds);
	const std::map<RsGxsGroupId, std::vector<RsGxsMessageId> > &getMsgIds();
	const std::map<RsGxsGroupId, std::vector<RsGxsMessageId> > &getMsgIdsMeta();
	void getAllMsgIds(std::map<RsGxsGroupId, std::vector<RsGxsMessageId> > &msgIds);

	RsGxsIfaceHelper *interfaceHelper() { return mInterfaceHelper; }

protected:
	virtual void showEvent(QShowEvent *event);

	// This is overloaded in subclasses.
	virtual void updateDisplay(bool complete) = 0;

private slots:
	void fillDisplay(bool complete);

private:
	RsGxsUpdateBroadcastBase *mBase;
	RsGxsIfaceHelper *mInterfaceHelper;
};
