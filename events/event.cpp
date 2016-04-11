/******************************************************************************
 * Copyright (C) 2015 Felix Rohrbach <kde@fxrh.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "event.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>

#include "../logging_util.h"
#include "roommessageevent.h"
#include "roomnameevent.h"
#include "roomaliasesevent.h"
#include "roomcanonicalaliasevent.h"
#include "roommemberevent.h"
#include "roomtopicevent.h"
#include "typingevent.h"
#include "receiptevent.h"
#include "unknownevent.h"

using namespace QMatrixClient;

class Event::Private
{
    public:
        EventType type;
        QString id;
        QDateTime timestamp;
        QString roomId;
        QString originalJson;
};

Event::Event(EventType type)
    : d(new Private)
{
    d->type = type;
}

Event::~Event()
{
    delete d;
}

EventType Event::type() const
{
    return d->type;
}

QString Event::id() const
{
    return d->id;
}

QDateTime Event::timestamp() const
{
    return d->timestamp;
}

QString Event::roomId() const
{
    return d->roomId;
}

QString Event::originalJson() const
{
    return d->originalJson;
}

// The below overloaded function constructs the needed Event descendant object
// using tail recursion to build a sequence of check-and-create-or-move-on actions.
// The first function sets the default: if nothing else matches,
// an UnknownEvent is constructed. The second one defines an induction step:
// it eats up its second (matching pattern for an event type in JSON) and third
// argument (actual factory function to call); checks whether the analyzed event
// in JSON matches the second argument; if it does, the third argument is
// invoked; if there's no match then the two used arguments are dropped and the
// function is reinvoked with the argument list without 2nd and 3rd argument.
// The number of arguments should be odd (1 + 2n), otherwise building will fail.
Event *makeEvent(const QJsonObject &obj)
{
    return UnknownEvent::fromJson(obj);
}

template <typename StrT, typename EvProducerT, typename... EvProducers>
Event *makeEvent(const QJsonObject &obj,
        StrT eventTypeStr, EvProducerT evProducer,
        EvProducers... otherProducers)
{
    if( obj.value("type").toString() == eventTypeStr )
        return evProducer(obj);
    return makeEvent(obj, otherProducers...);
}

// Using makeEvent() overloads above, it becomes very easy to create a map of
// event types to event object factories.
Event* Event::fromJson(const QJsonObject& obj)
{
    //qDebug() << obj.value("type").toString();
    return
        makeEvent(obj
            , "m.room.message", RoomMessageEvent::fromJson
            , "m.room.name", RoomNameEvent::fromJson
            , "m.room.aliases", RoomAliasesEvent::fromJson
            , "m.room.canonical_alias", RoomCanonicalAliasEvent::fromJson
            , "m.room.member", RoomMemberEvent::fromJson
            , "m.room.topic", RoomTopicEvent::fromJson
            , "m.typing", TypingEvent::fromJson
            , "m.receipt", ReceiptEvent::fromJson
            /* Insert new event types BEFORE this line */
        );
}

bool Event::parseJson(const QJsonObject& obj)
{
    d->originalJson = QString::fromUtf8(QJsonDocument(obj).toJson());
    bool correct = (d->type != EventType::Unknown);
    if ( d->type != EventType::Unknown && d->type != EventType::Typing )
    {
        if( obj.contains("event_id") )
        {
            d->id = obj.value("event_id").toString();
        } else {
            correct = false;
            qDebug() << "Event: can't find event_id";
            qDebug() << formatJson << d->originalJson;
        }
        if( obj.contains("origin_server_ts") )
        {
            d->timestamp = QDateTime::fromMSecsSinceEpoch( 
                static_cast<qint64>(obj.value("origin_server_ts").toDouble()), Qt::UTC );
        } else {
            correct = false;
            qDebug() << "Event: can't find ts";
            qDebug() << formatJson << d->originalJson;
        }
    }
    if( obj.contains("room_id") )
    {
        d->roomId = obj.value("room_id").toString();
    }
    return correct;
}

QList<Event*> QMatrixClient::eventListFromJson(const QJsonArray& json)
{
    QList<Event*> l;
    l.reserve(json.size());
    for (auto event: json)
        l.push_back(Event::fromJson(event.toObject()));
    return l;
}
