/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2006 Tatsuhiro Tsujikawa
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
/* copyright --> */
#include "HttpResponseCommand.h"
#include "DownloadEngine.h"
#include "SingleFileDownloadContext.h"
#include "FileEntry.h"
#include "RequestGroup.h"
#include "ServerHost.h"
#include "RequestGroupMan.h"
#include "Request.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "HttpConnection.h"
#include "TransferEncoding.h"
#include "SegmentMan.h"
#include "Segment.h"
#include "HttpDownloadCommand.h"
#include "DiskAdaptor.h"
#include "PieceStorage.h"
#include "DefaultBtProgressInfoFile.h"
#include "DownloadFailureException.h"
#include "DlAbortEx.h"
#include "Util.h"
#include "File.h"
#include "Option.h"
#include "Logger.h"
#include "Socket.h"
#include "message.h"
#include "prefs.h"
#include "StringFormat.h"
#include "HttpNullDownloadCommand.h"

namespace aria2 {

HttpResponseCommand::HttpResponseCommand(int32_t cuid,
					 const RequestHandle& req,
					 RequestGroup* requestGroup,
					 const HttpConnectionHandle& httpConnection,
					 DownloadEngine* e,
					 const SocketHandle& s)
  :AbstractCommand(cuid, req, requestGroup, e, s),
   httpConnection(httpConnection) {}

HttpResponseCommand::~HttpResponseCommand() {}

bool HttpResponseCommand::executeInternal()
{
  HttpRequestHandle httpRequest = httpConnection->getFirstHttpRequest();
  HttpResponseHandle httpResponse = httpConnection->receiveResponse();
  if(httpResponse.isNull()) {
    // The server has not responded to our request yet.
    e->commands.push_back(this);
    return false;
  }
  // check HTTP status number
  httpResponse->validateResponse();
  httpResponse->retrieveCookie();
  // check whether Location header exists. If it does, update request object
  // with redirected URL.
  if(httpResponse->isRedirect()) {
    // To reuse a connection, a response body must be received.
    if(req->supportsPersistentConnection() &&
       (httpResponse->getEntityLength() > 0 ||
	httpResponse->isTransferEncodingSpecified())) {
      return handleRedirect(httpResponse);
    } else {
      // Response body is 0 length or a response header shows that a persistent
      // connection is not enabled.
      if(req->supportsPersistentConnection()) {
	std::pair<std::string, uint16_t> peerInfo;
	socket->getPeerInfo(peerInfo);
	e->poolSocket(peerInfo.first, peerInfo.second, socket);
      }
      httpResponse->processRedirect();
      logger->info(MSG_REDIRECT, cuid, httpResponse->getRedirectURI().c_str());
      return prepareForRetry(0);
    }
  }
  if(!_requestGroup->isSingleHostMultiConnectionEnabled()) {
    _requestGroup->removeURIWhoseHostnameIs(_requestGroup->searchServerHost(cuid)->getHostname());
  }
  if(_requestGroup->getPieceStorage().isNull()) {
    uint64_t totalLength = httpResponse->getEntityLength();
    SingleFileDownloadContextHandle dctx =
      dynamic_pointer_cast<SingleFileDownloadContext>(_requestGroup->getDownloadContext());
    dctx->setTotalLength(totalLength);
    dctx->setFilename(httpResponse->determinFilename());
    dctx->setContentType(httpResponse->getContentType());
    _requestGroup->preDownloadProcessing();
    if(e->_requestGroupMan->isSameFileBeingDownloaded(_requestGroup)) {
      throw DownloadFailureException
	(StringFormat(EX_DUPLICATE_FILE_DOWNLOAD,
		      _requestGroup->getFilePath().c_str()).str());
    }
    if(totalLength == 0 || httpResponse->isTransferEncodingSpecified()) {
      // we ignore content-length when transfer-encoding is set
      dctx->setTotalLength(0);
      return handleOtherEncoding(httpResponse);
    } else {
      return handleDefaultEncoding(httpResponse);
    }
  } else {
    // validate totalsize
    _requestGroup->validateTotalLength(httpResponse->getEntityLength());
    e->commands.push_back(createHttpDownloadCommand(httpResponse));
    return true;
  }
}

bool HttpResponseCommand::handleDefaultEncoding(const HttpResponseHandle& httpResponse)
{
  HttpRequestHandle httpRequest = httpResponse->getHttpRequest();
  _requestGroup->initPieceStorage();

  // quick hack for method 'head',, is it necessary?
  if(httpRequest->getMethod() == Request::METHOD_HEAD) {
    // TODO because we don't want segment file to be saved.
    return true;
  }

  BtProgressInfoFileHandle infoFile(new DefaultBtProgressInfoFile(_requestGroup->getDownloadContext(), _requestGroup->getPieceStorage(), e->option));
  if(!infoFile->exists() && _requestGroup->downloadFinishedByFileLength()) {
    return true;
  }

  DownloadCommand* command = 0;
  try {
    _requestGroup->loadAndOpenFile(infoFile);
    File file(_requestGroup->getFilePath());

    SegmentHandle segment = _requestGroup->getSegmentMan()->getSegment(cuid, 0);
    // pipelining requires implicit range specified. But the request for
    // this response most likely dones't contains range header. This means
    // we can't continue to use this socket because server sends all entity
    // body instead of a segment.
    // Therefore, we shutdown the socket here if pipelining is enabled.
    if(!segment.isNull() && segment->getPositionToWrite() == 0 &&
       !req->isPipeliningEnabled()) {
      command = createHttpDownloadCommand(httpResponse);
    } else {
      _requestGroup->getSegmentMan()->cancelSegment(cuid);
    }
    prepareForNextAction(command);
  } catch(Exception& e) {
    delete command;
    throw;
  }
  return true;
}

bool HttpResponseCommand::handleOtherEncoding(const HttpResponseHandle& httpResponse) {
  HttpRequestHandle httpRequest = httpResponse->getHttpRequest();
  // quick hack for method 'head',, is it necessary?
  if(httpRequest->getMethod() == Request::METHOD_HEAD) {
    return true;
  }
  _requestGroup->initPieceStorage();
  _requestGroup->shouldCancelDownloadForSafety();
  _requestGroup->getPieceStorage()->getDiskAdaptor()->initAndOpenFile();
  e->commands.push_back(createHttpDownloadCommand(httpResponse));
  return true;
}

static SharedHandle<TransferEncoding> getTransferEncoding
(const SharedHandle<HttpResponse>& httpResponse)
{
  TransferEncodingHandle enc;
  if(httpResponse->isTransferEncodingSpecified()) {
    enc = httpResponse->getTransferDecoder();
    if(enc.isNull()) {
      throw DlAbortEx
	(StringFormat(EX_TRANSFER_ENCODING_NOT_SUPPORTED,
		      httpResponse->getTransferEncoding().c_str()).str());
    }
    enc->init();
  }
  return enc;
}

bool HttpResponseCommand::handleRedirect
(const SharedHandle<HttpResponse>& httpResponse)
{
  SharedHandle<TransferEncoding> enc(getTransferEncoding(httpResponse));
  HttpNullDownloadCommand* command = new HttpNullDownloadCommand
    (cuid, req, _requestGroup, httpConnection, httpResponse, e, socket);
  command->setTransferDecoder(enc);
  e->commands.push_back(command);
  return true;
}

HttpDownloadCommand* HttpResponseCommand::createHttpDownloadCommand(const HttpResponseHandle& httpResponse)
{
  TransferEncodingHandle enc(getTransferEncoding(httpResponse));
  HttpDownloadCommand* command =
    new HttpDownloadCommand(cuid, req, _requestGroup, httpConnection, e, socket);
  command->setMaxDownloadSpeedLimit(e->option->getAsInt(PREF_MAX_DOWNLOAD_LIMIT));
  command->setStartupIdleTime(e->option->getAsInt(PREF_STARTUP_IDLE_TIME));
  command->setLowestDownloadSpeedLimit(e->option->getAsInt(PREF_LOWEST_SPEED_LIMIT));
  command->setTransferDecoder(enc);

  return command;
}

} // namespace aria2
