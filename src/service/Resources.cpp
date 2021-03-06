/*
 *   Copyright (C) 2010 - 2016 by Ivan Cukic <ivan.cukic@kde.org>
 *
 *   This program is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU General Public License as
 *   published by the Free Software Foundation; either version 2 of
 *   the License or (at your option) version 3 or any later version
 *   accepted by the membership of KDE e.V. (or its successor approved
 *   by the membership of KDE e.V.), which shall act as a proxy
 *   defined in Section 14 of version 3 of the license.
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
#include "Resources.h"
#include "Resources_p.h"

// Qt
#include <QDBusConnection>
#include <QThread>
#include <QMutex>
#include <QMutexLocker>

// KDE
#include <kwindowsystem.h>
#include <kdbusconnectionpool.h>

// Utils
#include <utils/d_ptr_implementation.h>
#include <utils/remove_if.h>

// System
#include <time.h>

// Local
#include "Application.h"
#include "Activities.h"
#include "resourcesadaptor.h"
#include "common/dbus/common.h"


Resources::Private::Private(Resources *parent)
    : QThread(parent)
    , focussedWindow(0)
    , q(parent)
{
}

Resources::Private::~Private()
{
    requestInterruption();
    wait(1500); // Enough time for the sleep(1) + processing in run()
}

namespace {
EventList events;
QMutex events_mutex;
}

void Resources::Private::run()
{
    while (!isInterruptionRequested()) {
        // initial delay before processing the events
        sleep(1);

        EventList currentEvents;

        {
            QMutexLocker locker(&events_mutex);

            if (events.count() == 0) {
                return;
            }

            std::swap(currentEvents, events);
        }

        emit q->ProcessedResourceEvents(currentEvents);
    }
}

void Resources::Private::insertEvent(const Event &newEvent)
{
    if (lastEvent == newEvent) {
        return;
    }

    lastEvent = newEvent;

    {
        QMutexLocker locker(&events_mutex);
        events << newEvent;
    }

    emit q->RegisteredResourceEvent(newEvent);
}

void Resources::Private::addEvent(const QString &application, WId wid,
                                  const QString &uri, int type)
{
    Event newEvent(application, wid, uri, type);
    addEvent(newEvent);
}

void Resources::Private::addEvent(const Event &newEvent)
{
    // And now, for something completely delayed
    {
        QMutexLocker locker(&events_mutex);

        // Deleting previously registered Accessed events if
        // the current one has the same application and uri
        if (newEvent.type != Event::Accessed) {
            kamd::utils::remove_if(events, [&newEvent](const Event &event)->bool {
                return
                    event.application == newEvent.application &&
                    event.uri         == newEvent.uri
                ;
            });
        }
    }

    // Process the windowing
    // Essentially, this is the brain of SLC. We need to track the
    // window focus changes to be able to generate the potential
    // missing events like FocussedOut before Closed and similar.
    // So, there is no point in having the same logic in SLC plugin
    // as well.

    if (newEvent.wid != 0) {
        WindowData &window = windows[newEvent.wid];
        const QString &uri = newEvent.uri;

        window.application = newEvent.application;

        switch (newEvent.type) {
            case Event::Opened:
                insertEvent(newEvent);

                if (window.focussedResource.isEmpty()) {
                    // This window haven't had anything focused,
                    // assuming the new document is focused

                    window.focussedResource = newEvent.uri;
                    insertEvent(newEvent.deriveWithType(Event::FocussedIn));
                }

                break;

            case Event::FocussedIn:

                if (!window.resources.contains(uri)) {
                    // This window did not contain this resource before,
                    // sending Opened event

                    insertEvent(newEvent.deriveWithType(Event::Opened));
                }

                window.focussedResource = newEvent.uri;
                insertEvent(newEvent);

                break;

            case Event::Closed:

                if (window.focussedResource == uri) {
                    // If we are closing a document that is in focus,
                    // release focus first

                    insertEvent(newEvent.deriveWithType(Event::FocussedOut));
                    window.focussedResource.clear();
                }

                insertEvent(newEvent);

                break;

            case Event::FocussedOut:

                if (window.focussedResource == uri) {
                    window.focussedResource.clear();
                }

                insertEvent(newEvent);

                break;

            default:
                insertEvent(newEvent);
                break;
        }

    } else {
        // If we haven't got a window, just pass the event on,
        // but only if it is not a focus event
        if (newEvent.type != Event::FocussedIn
            && newEvent.type != Event::FocussedOut) {
            insertEvent(newEvent);
        }
    }

    start();
}

void Resources::Private::windowClosed(WId windowId)
{
    // Testing whether the window is a registered one

    if (!windows.contains(windowId)) {
        return;
    }

    if (focussedWindow == windowId) {
        focussedWindow = 0;
    }

    // Closing all the resources that the window registered

    for (const QString &uri: windows[windowId].resources) {
        q->RegisterResourceEvent(windows[windowId].application,
                                 windowId, uri, Event::Closed);
    }

    windows.remove(windowId);
}

void Resources::Private::activeWindowChanged(WId windowId)
{
    // If the focused window has changed, we need to create a
    // FocussedOut event for the resource it contains,
    // and FocussedIn for the resource of the new active window.
    // The windows can do this manually, but if they are
    // SDI, we can do it on our own.

    if (windowId == focussedWindow) {
        return;
    }

    if (windows.contains(focussedWindow)) {
        const WindowData &data = windows[focussedWindow];

        if (!data.focussedResource.isEmpty()) {
            insertEvent(Event(data.application, focussedWindow, data.focussedResource, Event::FocussedOut));
        }
    }

    focussedWindow = windowId;

    if (windows.contains(focussedWindow)) {
        const WindowData &data = windows[focussedWindow];

        if (!data.focussedResource.isEmpty()) {
            insertEvent(Event(data.application, windowId, data.focussedResource, Event::FocussedIn));
        }
    }
}

Resources::Resources(QObject *parent)
    : Module(QStringLiteral("resources"), parent)
    , d(this)
{
    qRegisterMetaType<Event>("Event");
    qRegisterMetaType<EventList>("EventList");
    qRegisterMetaType<WId>("WId");

    new ResourcesAdaptor(this);
    KDBusConnectionPool::threadConnection().registerObject(
        KAMD_DBUS_OBJECT_PATH(Resources), this);

    connect(KWindowSystem::self(), &KWindowSystem::windowRemoved,
            d.operator->(),        &Resources::Private::windowClosed);
    connect(KWindowSystem::self(), &KWindowSystem::activeWindowChanged,
            d.operator->(),        &Resources::Private::activeWindowChanged);
}

Resources::~Resources()
{
}

void Resources::RegisterResourceEvent(const QString &application, uint _windowId,
                                      const QString &uri, uint event)
{
    if (event > Event::LastEventType
        || uri.isEmpty()
        || application.isEmpty()) {
        return;
    }

    WId windowId = (WId)_windowId;

    d->addEvent(application, windowId, uri, (Event::Type)event);
}

void Resources::RegisterResourceMimetype(const QString &uri, const QString &mimetype)
{
    if (!mimetype.isEmpty()) {
        return;
    }

    emit RegisteredResourceMimetype(uri, mimetype);
}

void Resources::RegisterResourceTitle(const QString &uri, const QString &title)
{
    // A dirty saninty check for the title
    if (title.length() < 3) {
        return;
    }

    emit RegisteredResourceTitle(uri, title);
}

