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

#ifndef HOST_BROWSER_MODEL_H
#define HOST_BROWSER_MODEL_H

#include <QFileSystemModel>

class Client;

// A QFileSystemModel model that is capable of receiving DS3Browser drag/drops
class HostBrowserModel : public QFileSystemModel
{
public:
	HostBrowserModel(Client* client, QObject* parent = 0);

	bool dropMimeData(const QMimeData* data,
			  Qt::DropAction action,
			  int row, int column,
			  const QModelIndex& parent);
	QStringList mimeTypes() const;

private:
	Client* m_client;
};

#endif
