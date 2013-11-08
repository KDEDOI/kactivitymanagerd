/*
 *   Copyright (C) 2011, 2012 Ivan Cukic ivan.cukic(at)kde.org
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License as
 *   published by the Free Software Foundation; either version 2 of
 *   the License, or (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// Self
#include "Rankings.h"

// STL
#include <algorithm>

// Qt
#include <QDBusConnection>
#include <QVariantList>
#include <QSqlQuery>

// KDE
#include <kdbusconnectionpool.h>

// Utils
#include <utils/remove_if.h>
#include <utils/qsqlquery.h>

// Local
#include "Debug.h"
#include "ResourceScoreCache.h"
#include "DatabaseConnection.h"
#include "StatsPlugin.h"
#include "rankingsadaptor.h"


#define RESULT_COUNT_LIMIT 10
#define COALESCE_ACTIVITY(Activity) \
    ((Activity.isEmpty()) ? (StatsPlugin::self()->currentActivity()) : (Activity))

Rankings *Rankings::s_instance = Q_NULLPTR;

/**
 *
 */
RankingsUpdateThread::RankingsUpdateThread(
    const QString &activity, QVector<Rankings::ResultItem> &listptr,
    QHash<Rankings::Activity, qreal> &scoreTrashold)
    : m_activity(activity)
    , m_listptr(listptr)
    , m_scoreTrashold(scoreTrashold)
{
}

RankingsUpdateThread::~RankingsUpdateThread()
{
}

void RankingsUpdateThread::run()
{
    static const auto query = QStringLiteral(
        "SELECT targettedResource, cachedScore "
        "FROM kext_ResourceScoreCache " // this should be kao_ResourceScoreCache, but lets leave it
        "WHERE usedActivity = '%1' "
        "AND cachedScore > 0 "
        "ORDER BY cachedScore DESC LIMIT 30");

    auto results = DatabaseConnection::exec(query.arg(m_activity));

    for (const auto &result: results) {
        const auto url = result[0].toString();
        const auto score = result[1].toReal();

        if (score > m_scoreTrashold[m_activity]) {
            m_listptr << Rankings::ResultItem(url, score);
        }
    }

    emit loaded(m_activity);
}

void Rankings::init(QObject *parent)
{
    if (s_instance) {
        return;
    }

    s_instance = new Rankings(parent);
}

Rankings *Rankings::self()
{
    return s_instance;
}

Rankings::Rankings(QObject *parent)
    : QObject(parent)
{
    new RankingsAdaptor(this);
    KDBusConnectionPool::threadConnection().registerObject(
        QStringLiteral("/Rankings"), this);

    initResults(QString());
}

Rankings::~Rankings()
{
}

void Rankings::registerClient(const QString &client,
                              const QString &activity, const QString &type)
{
    Q_UNUSED(type);

    if (!m_clients.contains(activity)) {
        initResults(COALESCE_ACTIVITY(activity));
    }

    if (!m_clients[activity].contains(client)) {
        m_clients[activity] << client;
    }

    notifyResultsUpdated(activity, QStringList() << client);
}

void Rankings::deregisterClient(const QString &client)
{
    QMutableHashIterator<Activity, QStringList> i(m_clients);

    while (i.hasNext()) {
        i.next();

        i.value().removeAll(client);
        if (i.value().isEmpty()) {
            i.remove();
        }
    }
}

void Rankings::setCurrentActivity(const QString &activity)
{
    // We need to update scores for items that have no
    // activity specified

    initResults(activity);
}

void Rankings::initResults(const QString &_activity)
{
    const auto activity = COALESCE_ACTIVITY(_activity);

    m_results[activity].clear();
    notifyResultsUpdated(activity);
    updateScoreTrashold(activity);

    const auto thread = new RankingsUpdateThread(
        activity, m_results[activity], m_resultScoreTreshold);

    connect(thread, SIGNAL(loaded(QString)),
            this, SLOT(notifyResultsUpdated(QString)));
    connect(thread, SIGNAL(terminated()),
            thread, SLOT(deleteLater()));

    thread->start();
}

void Rankings::resourceScoreUpdated(const QString &activity,
                                    const QString &application,
                                    const QString &uri, qreal score)
{
    Q_UNUSED(application);

    if (score <= m_resultScoreTreshold[activity]) {
        return;
    }

    auto &results = m_results[activity];

    // Removing the item from the list if it is already in it

    kamd::utils::remove_if(results, [&uri](const ResultItem &item) {
        return item.uri == uri;
    });

    // Adding the item

    ResultItem item(uri, score);

    auto insertionPoint
        = std::lower_bound(results.begin(), results.end(), item);

    results.insert(insertionPoint, item);

    results.resize(std::min(results.size(), RESULT_COUNT_LIMIT));

    notifyResultsUpdated(activity);
}

void Rankings::updateScoreTrashold(const QString &activity)
{
    m_resultScoreTreshold[activity]
        = (m_results[activity].size() >= RESULT_COUNT_LIMIT)
              ? m_results[activity].last().score
              : 0;
}

void Rankings::notifyResultsUpdated(const QString &_activity,
                                    QStringList clients)
{
    const auto activity = COALESCE_ACTIVITY(_activity);

    updateScoreTrashold(activity);

    QVariantList data;
    for (const auto &item : m_results[activity]) {
        data << item.uri;
    }

    if (clients.isEmpty()) {
        clients = m_clients[activity];

        if (activity == StatsPlugin::self()->currentActivity()) {
            clients.append(m_clients[QString()]);
        }
    }

    // TODO: We don't really have users of this one
    // If we get them, enable this again:
    for (const auto &client : clients) {
        QDBusInterface(
            client, QStringLiteral("/RankingsClient"),
            QStringLiteral("org.kde.ActivityManager.RankingsClient")
        ).asyncCall(QStringLiteral("updated"), data);
    }
}

void Rankings::requestScoreUpdate(const QString &activity,
                                  const QString &application,
                                  const QString &resource)
{
    ResourceScoreCache(activity, application, resource).updateScore();
}
