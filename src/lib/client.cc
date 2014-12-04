/*
 * *****************************************************************************
 *   Copyright 2014 Spectra Logic Corporation. All Rights Reserved.
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

#include <stdlib.h>
#include <QtConcurrent>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>

#include "lib/work_items/bulk_get_work_item.h"
#include "lib/work_items/bulk_put_work_item.h"
#include "lib/work_items/object_work_item.h"
#include "lib/client.h"
#include "lib/logger.h"
#include "models/ds3_url.h"
#include "models/session.h"

using QtConcurrent::run;

const QString Client::DELIMITER = "/";

// The S3 server imposes this limit although we might want to lower it
const uint64_t Client::BULK_PAGE_LIMIT = 500000;

static size_t read_from_file(void* buffer, size_t size, size_t count, void* user_data);

// Simple struct to wrap a Client and an ObjectWorkItem so the C SDK can
// send both to the file read/write callback functions.
struct ClientAndObjectWorkItem
{
	Client* client;
	ObjectWorkItem* objectWorkItem;
};

Client::Client(const Session* session)
{
	m_creds = ds3_create_creds(session->GetAccessId().toUtf8().constData(),
				   session->GetSecretKey().toUtf8().constData());

	QString protocol = session->GetProtocolName();
	m_host = session->GetHost();
	m_endpoint = protocol + "://" + m_host;
	QString port = session->GetPort();
	if (!port.isEmpty() && port != "80" && port != "443") {
		m_endpoint += ":" + port;
	}

	m_client = ds3_create_client(m_endpoint.toUtf8().constData(), m_creds);
	QString proxy = session->GetProxy();
	if (!proxy.isEmpty()) {
		ds3_client_proxy(m_client, proxy.toUtf8().constData());
	}
}

Client::~Client()
{
	ds3_free_creds(m_creds);
	ds3_free_client(m_client);
	ds3_cleanup();
}

QFuture<ds3_get_service_response*>
Client::GetService()
{
	QFuture<ds3_get_service_response*> future = run(this, &Client::DoGetService);
	return future;
}

QFuture<ds3_get_bucket_response*>
Client::GetBucket(const QString& bucketName, const QString& prefix,
		  const QString& marker, uint32_t maxKeys)
{
	QFuture<ds3_get_bucket_response*> future = run(this,
						       &Client::DoGetBucket,
						       bucketName,
						       prefix,
						       marker,
						       maxKeys);
	return future;
}

void
Client::CreateBucket(const QString& name)
{
	ds3_request* request = ds3_init_put_bucket(name.toUtf8().constData());
	LOG_INFO("Create Bucket " + name + " (PUT " + m_endpoint + "/" + \
		 name + ")");
	ds3_error* error = ds3_put_bucket(m_client, request);
	ds3_free_request(request);

	if (error) {
		// TODO Handle the error
		ds3_free_error(error);
	}
}

void
Client::BulkGet(const QList<QUrl> urls, const QString& destination)
{
	BulkGetWorkItem* workItem = new BulkGetWorkItem(m_host, urls,
							destination);
	workItem->SetState(Job::QUEUED);
	Job job = workItem->ToJob();
	emit JobProgressUpdate(job);
	run(this, &Client::PrepareBulkGets, workItem);
}

void
Client::BulkPut(const QString& bucketName,
		const QString& prefix,
		const QList<QUrl> urls)
{
	BulkPutWorkItem* workItem = new BulkPutWorkItem(m_host, urls,
							bucketName, prefix);
	workItem->SetState(Job::QUEUED);
	Job job = workItem->ToJob();
	emit JobProgressUpdate(job);
	run(this, &Client::PrepareBulkPuts, workItem);
}

void
Client::GetObject(const QString& /*bucket*/,
		  const QString& object,
		  const QString& destination,
		  BulkGetWorkItem* /*bulkGetWorkItem*/)
{
	LOG_DEBUG("GetObject " + object + " to " + destination);
}

void
Client::PutObject(const QString& bucket,
		  const QString& object,
		  const QString& fileName,
		  BulkPutWorkItem* bulkPutWorkItem)
{
	QFileInfo fileInfo(fileName);
	ds3_request* request = ds3_init_put_object(bucket.toUtf8().constData(),
						   object.toUtf8().constData(),
						   fileInfo.size());
	ds3_error* error = NULL;
	if (fileInfo.isDir()) {
		// "folder" objects don't have a size nor do they have any
		// data associated with them
		error = ds3_put_object(m_client, request, NULL, NULL);
	} else {
		QFile file(fileName);
		ObjectWorkItem objWorkItem(bucket, object, fileName, bulkPutWorkItem);
		ClientAndObjectWorkItem caowi;
		caowi.client = this;
		caowi.objectWorkItem = &objWorkItem;
		if (objWorkItem.OpenFile(QIODevice::ReadOnly)) {
			error = ds3_put_object(m_client, request,
					       &caowi,
					       read_from_file);
		} else {
			LOG_ERROR("PUT object failed: unable to open file " + fileName);
		}
	}
	ds3_free_request(request);

	if (error) {
		// TODO Handle the error
		ds3_free_error(error);
	}
}

ds3_get_service_response*
Client::DoGetService()
{
	ds3_get_service_response *response;
	ds3_request* request = ds3_init_get_service();
	LOG_INFO("Get Buckets (GET " + m_endpoint + ")");
	ds3_error* error = ds3_get_service(m_client,
					   request,
					   &response);
	ds3_free_request(request);

	if (error) {
		// TODO Handle the error
		ds3_free_error(error);
	}

	return response;
}

ds3_get_bucket_response*
Client::DoGetBucket(const QString& bucketName, const QString& prefix,
		    const QString& marker, uint32_t maxKeys)
{
	ds3_get_bucket_response *response;
	ds3_request* request = ds3_init_get_bucket(bucketName.toUtf8().constData());
	QString logMsg = "List Objects (GET " + m_endpoint + "/";
	logMsg += bucketName;
	QStringList logQueryParams;
	if (!prefix.isEmpty()) {
		ds3_request_set_prefix(request, prefix.toUtf8().constData());
		logQueryParams << "prefix=" + prefix;
	}
	ds3_request_set_delimiter(request, DELIMITER.toUtf8().constData());
	logQueryParams << "delimiter=" + DELIMITER;
	if (!marker.isEmpty()) {
		ds3_request_set_marker(request, marker.toUtf8().constData());
		logQueryParams << "marker=" + marker;
	}
	if (maxKeys > 0) {
		ds3_request_set_max_keys(request, maxKeys);
		logQueryParams << "max-keys=" + QString::number(maxKeys);
	}
	if (!logQueryParams.isEmpty()) {
		logMsg += "&" + logQueryParams.join("&");
	}
	logMsg += ")";
	LOG_INFO(logMsg);
	ds3_error* error = ds3_get_bucket(m_client,
					  request,
					  &response);
	ds3_free_request(request);

	if (error) {
		// TODO Handle the error
		ds3_free_error(error);
	}

	return response;
}

void
Client::PrepareBulkGets(BulkGetWorkItem* workItem)
{
	LOG_DEBUG("PrepareBulkGets");

	workItem->SetState(Job::PREPARING);
	Job job = workItem->ToJob();
	emit JobProgressUpdate(job);

	workItem->ClearObjMap();

	QString prevBucket;

	for (QList<QUrl>::const_iterator& ui(workItem->GetUrlsIterator());
	     ui != workItem->GetUrlsConstEnd();
	     ui++) {
		DS3URL url(*ui);
		QString bucket = url.GetBucketName();
		workItem->SetBucketName(bucket);
		if (workItem->GetObjMapSize() >= BULK_PAGE_LIMIT ||
		    (!prevBucket.isEmpty() && prevBucket != bucket)) {
			run(this, &Client::DoBulkGet, workItem);
			return;
		}

		QString fullObjName = url.GetObjectName();
		QString lastPathPart = url.GetLastPathPart();
		QString filePath = QDir::cleanPath(workItem->GetDestination() +
						   "/" + lastPathPart);
		if (url.IsBucketOrFolder()) {
		// 	get all objects underneath and add to the objmap.
		//
		//	if (no objects underneath) {
		//		directly create the folder
		//	}
		} else {
			workItem->InsertObjMap(fullObjName, filePath);
		}

		prevBucket = bucket;
	}

	if (workItem->GetObjMapSize() > 0) {
		run(this, &Client::DoBulkGet, workItem);
	}
}

void
Client::DoBulkGet(BulkGetWorkItem* workItem)
{
	LOG_DEBUG("DoBulkGets");

	workItem->SetState(Job::INPROGRESS);
	workItem->SetTransferStartIfNull();
	Job job = workItem->ToJob();
	emit JobProgressUpdate(job);

	uint64_t numObjs = workItem->GetObjMapSize();
	ds3_bulk_object_list *bulkObjList = ds3_init_bulk_object_list(numObjs);

	QHash<QString, QString>::const_iterator hi;
	int i = 0;
	for (hi = workItem->GetObjMapConstBegin(); hi != workItem->GetObjMapConstEnd(); hi++) {
		ds3_bulk_object* bulkObj = &bulkObjList->list[i];
		QString objName = hi.key();
		QString filePath = hi.value();
		QFileInfo fileInfo(filePath);
		bulkObj->name = ds3_str_init(objName.toUtf8().constData());
		i++;
	}

	const QString& bucketName = workItem->GetBucketName();
	ds3_request* request = ds3_init_get_bulk(bucketName.toUtf8().constData(), bulkObjList);
	ds3_bulk_response *response = NULL;
	ds3_error* error = ds3_bulk(m_client, request, &response);
	ds3_free_request(request);
	ds3_free_bulk_object_list(bulkObjList);
	workItem->SetResponse(response);

	if (error) {
		// TODO Handle the error
		LOG_ERROR("BulkGet Error");
		ds3_free_error(error);
	}

	if (response == NULL || (response != NULL && response->list_size == 0)) {
		//DeleteOrRequeueBulkGetWorkItem(workItem);
		return;
	}

	for (size_t j = 0; j < response->list_size; j++) {
		LOG_DEBUG("Starting GetBulkOjbectList thread");
		ds3_bulk_object_list* list = response->list[j];
		workItem->IncWorkingObjListCount();
		run(this, &Client::GetBulkObjectList, workItem, list);
	}
}

void
Client::GetBulkObjectList(BulkGetWorkItem* workItem,
			  const ds3_bulk_object_list* list)
{
	QString bucketName = workItem->GetBucketName();
	for (uint64_t k = 0; k < list->size; k++) {
		ds3_bulk_object* bulkObj = &list->list[k];
		QString objName = QString(bulkObj->name->value);
		QString filePath = workItem->GetObjMapValue(objName);
		GetObject(bucketName, objName, filePath, workItem);
	}
	workItem->DecWorkingObjListCount();
	//DeleteOrRequeueBulkPutWorkItem(workItem);
}

void
Client::PrepareBulkPuts(BulkPutWorkItem* workItem)
{
	LOG_DEBUG("PrepareBulkPuts");

	workItem->SetState(Job::PREPARING);
	Job job = workItem->ToJob();
	emit JobProgressUpdate(job);

	workItem->ClearObjMap();
	QString normPrefix = workItem->GetPrefix();
	if (!normPrefix.isEmpty()) {
		normPrefix.replace(QRegExp("/$"), "");
		normPrefix += "/";
	}

	for (QList<QUrl>::const_iterator& ui(workItem->GetUrlsIterator());
	     ui != workItem->GetUrlsConstEnd();
	     ui++) {
		if (workItem->GetObjMapSize() >= BULK_PAGE_LIMIT) {
			run(this, &Client::DoBulkPut, workItem);
			return;
		}
		QString filePath = (*ui).toLocalFile();
		// filePath could be either /foo or /foo/ if it's a directory.
		// Run it through QDir to normalize it to the former.
		filePath = QDir(filePath).path();
		QFileInfo fileInfo(filePath);
		QString fileName = fileInfo.fileName();
		QString objName = normPrefix + fileName;
		if (fileInfo.isDir()) {
			objName += "/";

			// An existing DirIterator must have been caused by
			// a previous BulkPut "page" that returned early while
			// iterating over files under this URL.  Thus, take
			// over where it left off.
			QDirIterator* di = workItem->GetDirIterator();
			if (di == NULL) {
				di = workItem->GetDirIterator(filePath);
			}
			while (di->hasNext()) {
				if (workItem->GetObjMapSize() >= BULK_PAGE_LIMIT) {
					run(this, &Client::DoBulkPut, workItem);
					return;
				}
				QString subFilePath = di->next();
				QFileInfo subFileInfo = di->fileInfo();
				QString subFileName = subFilePath;
				subFileName.replace(QRegExp("^" + filePath + "/"), "");
				QString subObjName = objName + subFileName;
				if (subFileInfo.isDir()) {
					subObjName += "/";
				}
				workItem->InsertObjMap(subObjName, subFilePath);
			}
			workItem->DeleteDirIterator();
		}
		workItem->InsertObjMap(objName, filePath);
	}

	if (workItem->GetObjMapSize() > 0) {
		run(this, &Client::DoBulkPut, workItem);
	}
}

void
Client::DoBulkPut(BulkPutWorkItem* workItem)
{
	LOG_DEBUG("DoBulkPuts");

	workItem->SetState(Job::INPROGRESS);
	workItem->SetTransferStartIfNull();
	Job job = workItem->ToJob();
	emit JobProgressUpdate(job);

	uint64_t numFiles = workItem->GetObjMapSize();
	ds3_bulk_object_list *bulkObjList = ds3_init_bulk_object_list(numFiles);

	QHash<QString, QString>::const_iterator hi;
	int i = 0;
	for (hi = workItem->GetObjMapConstBegin(); hi != workItem->GetObjMapConstEnd(); hi++) {
		ds3_bulk_object* bulkObj = &bulkObjList->list[i];
		QString objName = hi.key();
		QString filePath = hi.value();
		QFileInfo fileInfo(filePath);
		uint64_t fileSize = 0;
		if (!fileInfo.isDir()) {
			fileSize = fileInfo.size();
		}
		bulkObj->name = ds3_str_init(objName.toUtf8().constData());
		bulkObj->length = fileSize;
		bulkObj->offset = 0;
		i++;
	}

	const QString& bucketName = workItem->GetBucketName();
	ds3_request* request = ds3_init_put_bulk(bucketName.toUtf8().constData(), bulkObjList);
	ds3_bulk_response *response = NULL;
	ds3_error* error = ds3_bulk(m_client, request, &response);
	ds3_free_request(request);
	ds3_free_bulk_object_list(bulkObjList);
	workItem->SetResponse(response);

	if (error) {
		// TODO Handle the error
		LOG_ERROR("BulkPut Error");
		ds3_free_error(error);
	}

	if (response == NULL || (response != NULL && response->list_size == 0)) {
		DeleteOrRequeueBulkPutWorkItem(workItem);
		return;
	}

	for (size_t j = 0; j < response->list_size; j++) {
		LOG_DEBUG("Starting PutBulkOjbectList thread");
		ds3_bulk_object_list* list = response->list[j];
		workItem->IncWorkingObjListCount();
		run(this, &Client::PutBulkObjectList, workItem, list);
	}
}

void
Client::PutBulkObjectList(BulkPutWorkItem* workItem,
			  const ds3_bulk_object_list* list)
{
	QString bucketName = workItem->GetBucketName();
	for (uint64_t k = 0; k < list->size; k++) {
		ds3_bulk_object* bulkObj = &list->list[k];
		QString objName = QString(bulkObj->name->value);
		QString filePath = workItem->GetObjMapValue(objName);
		PutObject(bucketName, objName, filePath, workItem);
	}
	workItem->DecWorkingObjListCount();
	DeleteOrRequeueBulkPutWorkItem(workItem);
}

void
Client::DeleteOrRequeueBulkPutWorkItem(BulkPutWorkItem* workItem)
{
	if (workItem->IsPageFinished()) {
		if (workItem->IsFinished()) {
			LOG_DEBUG("Finished with bulk put work item.  Deleting it.");
			workItem->SetState(Job::FINISHED);
			Job job = workItem->ToJob();
			emit JobProgressUpdate(job);
			delete workItem;
		} else {
			LOG_DEBUG("More bulk put pages to go.  Starting PrepareBulkPuts again.");
			run(this, &Client::PrepareBulkPuts, workItem);
		}
	} else {
		LOG_DEBUG("Page not finished.  objlistcount: " +
			  QString::number(workItem->GetWorkingObjListCount()));
	}
}

static size_t
read_from_file(void* buffer, size_t size, size_t count, void* user_data)
{
	ClientAndObjectWorkItem* caowi = static_cast<ClientAndObjectWorkItem*>(user_data);
	Client* client = caowi->client;
	ObjectWorkItem* workItem = caowi->objectWorkItem;
	return client->ReadFile(workItem, (char*)buffer, size, count);
}

size_t
Client::ReadFile(ObjectWorkItem* workItem, char* buffer,
		 size_t size, size_t count)
{
	size_t bytesRead = workItem->ReadFile(buffer, size, count);
	BulkWorkItem* bulkWorkItem = workItem->GetBulkWorkItem();
	if (bulkWorkItem != NULL) {
		Job job = bulkWorkItem->ToJob();
		emit JobProgressUpdate(job);
	}
	return bytesRead;
}
