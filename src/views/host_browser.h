/*
 * *****************************************************************************
 *   Copyright 2014-2015 Spectra Logic Corporation. All Rights Reserved.
 *   Licensed under the Apache License, Version 2.0 (the "License"). You may not
 *   use this file except in compliance with the License. A copy of the License
 *   is located at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 *   or in the "license" file accompanying this file.
 *   This file is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 *   CONDITIONS OF ANY KIND, either express or implied. See the License for the
 *   specific language governing permissions and limitations under the License.
 * *****************************************************************************
 */

#ifndef HOST_BROWSER_H
#define HOST_BROWSER_H

#include <QList>

#include "views/browser.h"

class HostBrowserModel;

// HostBrowser, a Browser class used for the local/host system
class HostBrowser : public Browser
{
	Q_OBJECT

public:
	HostBrowser(Client* client,
		    QWidget* parent = 0,
		    Qt::WindowFlags flags = 0);
	bool CanReceive(QModelIndex& index);
	void CanTransfer(bool enable);
	QModelIndexList GetSelected();
	void GetData(QMimeData* data);
	void SetViewRoot(const QModelIndex& index);

signals:
	void Transferable();
	void StartTransfer(QMimeData* data);

protected:
	void AddCustomToolBarActions();
	QString IndexToPath(const QModelIndex& index) const;
	void UpdatePathLabel(const QString& path);
	void OnContextMenuRequested(const QPoint& pos);
	void OnModelItemDoubleClick(const QModelIndex& index);

private:
	QAction* m_rootAction;
	QAction* m_homeAction;
	QAction* m_transferAction;
	HostBrowserModel* m_model;

protected slots:
	void OnModelItemClick(const QModelIndex& index);
	void PrepareTransfer();

private slots:
	void GoToHome();
};

#endif
