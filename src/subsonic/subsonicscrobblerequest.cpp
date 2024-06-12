/*
 * Strawberry Music Player
 * Copyright 2019-2021, Jonas Kvinge <jonas@jkvinge.net>
 * Copyright 2020-2021, Pascal Below <spezifisch@below.fr>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QDateTime>
#include <QNetworkReply>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>

#include "core/application.h"
#include "core/logging.h"
#include "subsonicservice.h"
#include "subsonicbaserequest.h"
#include "subsonicscrobblerequest.h"

namespace {
constexpr int kMaxConcurrentScrobbleRequests = 3;
}

SubsonicScrobbleRequest::SubsonicScrobbleRequest(SubsonicService *service, SubsonicUrlHandler *url_handler, Application *app, QObject *parent)
    : SubsonicBaseRequest(service, parent),
      service_(service),
      url_handler_(url_handler),
      app_(app),
      scrobble_requests_active_(0) {}

SubsonicScrobbleRequest::~SubsonicScrobbleRequest() {

  while (!replies_.isEmpty()) {
    QNetworkReply *reply = replies_.takeFirst();
    QObject::disconnect(reply, nullptr, this, nullptr);
    if (reply->isRunning()) reply->abort();
    reply->deleteLater();
  }

}

void SubsonicScrobbleRequest::CreateScrobbleRequest(const QString &song_id, const bool submission, const QDateTime &start_time) {

  Request request;
  request.song_id = song_id;
  request.submission = submission;
  request.time_ms = start_time.toMSecsSinceEpoch();
  scrobble_requests_queue_.enqueue(request);
  if (scrobble_requests_active_ < kMaxConcurrentScrobbleRequests) FlushScrobbleRequests();

}

void SubsonicScrobbleRequest::FlushScrobbleRequests() {

  while (!scrobble_requests_queue_.isEmpty() && scrobble_requests_active_ < kMaxConcurrentScrobbleRequests) {

    Request request = scrobble_requests_queue_.dequeue();
    ++scrobble_requests_active_;

    ParamList params = ParamList() << Param(QStringLiteral("id"), request.song_id)
                                   << Param(QStringLiteral("submission"), QVariant(request.submission).toString())
                                   << Param(QStringLiteral("time"), QVariant(request.time_ms).toString());

    QNetworkReply *reply = CreateGetRequest(QStringLiteral("scrobble"), params);
    replies_ << reply;
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() { ScrobbleReplyReceived(reply); });

  }

}

void SubsonicScrobbleRequest::ScrobbleReplyReceived(QNetworkReply *reply) {

  if (!replies_.contains(reply)) return;
  replies_.removeAll(reply);
  QObject::disconnect(reply, nullptr, this, nullptr);
  reply->deleteLater();

  --scrobble_requests_active_;

  // "subsonic-response" is empty on success, but some keys like status, version, or type might be present.
  // Therefore, we can only check for errors.
  QByteArray data = GetReplyData(reply);

  if (data.isEmpty()) {
    FinishCheck();
    return;
  }

  QJsonObject json_obj = ExtractJsonObj(data);
  if (json_obj.isEmpty()) {
    FinishCheck();
    return;
  }

  if (json_obj.contains(QLatin1String("error"))) {
    QJsonValue json_error = json_obj[QLatin1String("error")];
    if (!json_error.isObject()) {
      Error(QStringLiteral("Json error is not an object."), json_obj);
      FinishCheck();
      return;
    }
    json_obj = json_error.toObject();
    if (!json_obj.isEmpty() && json_obj.contains(QLatin1String("code")) && json_obj.contains(QLatin1String("message"))) {
      int code = json_obj[QLatin1String("code")].toInt();
      QString message = json_obj[QLatin1String("message")].toString();
      Error(QStringLiteral("%1 (%2)").arg(message).arg(code));
      FinishCheck();
    }
    else {
      Error(QStringLiteral("Json error object is missing code or message."), json_obj);
      FinishCheck();
      return;
    }
    return;
  }

  FinishCheck();

}

void SubsonicScrobbleRequest::FinishCheck() {

  if (!scrobble_requests_queue_.isEmpty() && scrobble_requests_active_ < kMaxConcurrentScrobbleRequests) FlushScrobbleRequests();

}

void SubsonicScrobbleRequest::Error(const QString &error, const QVariant &debug) {

  if (!error.isEmpty()) {
    qLog(Error) << "SubsonicScrobbleRequest:" << error;
    errors_ << error;
  }
  if (debug.isValid()) qLog(Debug) << debug;

  FinishCheck();

}
